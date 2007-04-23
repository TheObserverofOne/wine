/*
 * Alsa MIXER Wine Driver for Linux
 * Very loosely based on wineoss mixer driver
 *
 * Copyright 2007 Maarten Lankhorst
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"

#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#ifdef HAVE_SYS_IOCTL_H
# include <sys/ioctl.h>
#endif

#define NONAMELESSUNION
#define NONAMELESSSTRUCT

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winerror.h"
#include "winuser.h"
#include "winnls.h"
#include "mmddk.h"
#include "mmsystem.h"
#include "alsa.h"
#include "wine/unicode.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mixer);

#ifdef HAVE_ALSA

#define	WINE_MIXER_MANUF_ID      0xAA
#define	WINE_MIXER_PRODUCT_ID    0x55
#define	WINE_MIXER_VERSION       0x0100

/* Generic notes:
 * In windows it seems to be required for all controls to have a volume switch
 * In alsa that's optional
 *
 * I assume for playback controls, that there is always a playback volume switch available
 * Mute is optional
 *
 * For capture controls, it is needed that there is a capture switch and a volume switch,
 * It doesn't matter wether it is a playback volume switch or a capture volume switch.
 * The code will first try to get/adjust capture volume, if that fails it tries playback volume
 * It is not pretty, but under my 3 test cards it seems that there is no other choice:
 * Most capture controls don't have a capture volume setting
 *
 * MUX means that only capture source can be exclusively selected,
 * MIXER means that multiple sources can be selected simultaneously.
 */

static const char * getMessage(UINT uMsg)
{
    static char str[64];
#define MSG_TO_STR(x) case x: return #x;
    switch (uMsg){
    MSG_TO_STR(DRVM_INIT);
    MSG_TO_STR(DRVM_EXIT);
    MSG_TO_STR(DRVM_ENABLE);
    MSG_TO_STR(DRVM_DISABLE);
    MSG_TO_STR(MXDM_GETDEVCAPS);
    MSG_TO_STR(MXDM_GETLINEINFO);
    MSG_TO_STR(MXDM_GETNUMDEVS);
    MSG_TO_STR(MXDM_OPEN);
    MSG_TO_STR(MXDM_CLOSE);
    MSG_TO_STR(MXDM_GETLINECONTROLS);
    MSG_TO_STR(MXDM_GETCONTROLDETAILS);
    MSG_TO_STR(MXDM_SETCONTROLDETAILS);
    default: break;
    }
#undef MSG_TO_STR
    sprintf(str, "UNKNOWN(%08x)", uMsg);
    return str;
}

/* A simple declaration of a line control
 * These are each of the channels that show up
 */
typedef struct line {
    /* Name we present to outside world */
    WCHAR name[MAXPNAMELEN];

    DWORD component;
    DWORD dst;
    DWORD capt;
    DWORD chans;
    snd_mixer_elem_t *elem;
} line;

/* Mixer device */
typedef struct mixer
{
    snd_mixer_t *mix;
    WCHAR mixername[MAXPNAMELEN];

    int chans, dests;
    LPDRVCALLBACK callback;
    DWORD_PTR callbackpriv;
    HDRVR hmx;

    line *lines;
} mixer;

#define MAX_MIXERS 32

static int cards = 0;
static mixer mixdev[MAX_MIXERS];
static HANDLE thread;
static int elem_callback(snd_mixer_elem_t *elem, unsigned int mask);
static DWORD WINAPI ALSA_MixerPollThread(LPVOID lParam);
static CRITICAL_SECTION elem_crst;
static int msg_pipe[2];
static LONG refcnt;

/* found channel names in alsa lib, alsa api doesn't have another way for this
 * map name -> componenttype, worst case we get a wrong componenttype which is
 * mostly harmless
 */

static const struct mixerlinetype {
    const char *name;  DWORD cmpt;
} converttable[] = {
    { "Master",     MIXERLINE_COMPONENTTYPE_DST_SPEAKERS,    },
    { "Capture",    MIXERLINE_COMPONENTTYPE_DST_WAVEIN,      },
    { "PCM",        MIXERLINE_COMPONENTTYPE_SRC_WAVEOUT,     },
    { "PC Speaker", MIXERLINE_COMPONENTTYPE_SRC_PCSPEAKER,   },
    { "Synth",      MIXERLINE_COMPONENTTYPE_SRC_SYNTHESIZER, },
    { "Headphone",  MIXERLINE_COMPONENTTYPE_DST_HEADPHONES,  },
    { "Mic",        MIXERLINE_COMPONENTTYPE_SRC_MICROPHONE,  },
    { "Aux",        MIXERLINE_COMPONENTTYPE_SRC_UNDEFINED,   },
    { "CD",         MIXERLINE_COMPONENTTYPE_SRC_COMPACTDISC, },
    { "Line",       MIXERLINE_COMPONENTTYPE_SRC_LINE,        },
    { "Phone",      MIXERLINE_COMPONENTTYPE_SRC_TELEPHONE,   },
};

/* Map name to MIXERLINE_COMPONENTTYPE_XXX */
static int getcomponenttype(const char *name)
{
    int x;
    for (x=0; x< sizeof(converttable)/sizeof(converttable[0]); ++x)
        if (!strcasecmp(name, converttable[x].name))
        {
            TRACE("%d -> %s\n", x, name);
            return converttable[x].cmpt;
        }
    WARN("Unknown mixer name %s, probably harmless\n", name);
    return MIXERLINE_COMPONENTTYPE_SRC_UNDEFINED;
}

/* Is this control suited for showing up? */
static int blacklisted(snd_mixer_elem_t *elem)
{
    const char *name = snd_mixer_selem_get_name(elem);
    BOOL blisted = 0;

    if (!snd_mixer_selem_has_playback_volume(elem) &&
        (!snd_mixer_selem_has_capture_volume(elem) ||
         !snd_mixer_selem_has_capture_switch(elem)))
        blisted = 1;

    TRACE("%s: %x\n", name, blisted);
    return blisted;
}

/* get amount of channels for elem */
/* Officially we should keep capture/playback seperated,
 * but that's not going to work in the alsa api */
static int chans(mixer *mmixer, snd_mixer_elem_t * elem, DWORD capt)
{
    int ret=0, chn;

    if (capt && snd_mixer_selem_has_capture_volume(elem)) {
        for (chn = 0; chn <= SND_MIXER_SCHN_LAST; ++chn)
            if (snd_mixer_selem_has_capture_channel(elem, chn))
                ++ret;
    } else {
        for (chn = 0; chn <= SND_MIXER_SCHN_LAST; ++chn)
            if (snd_mixer_selem_has_playback_channel(elem, chn))
                ++ret;
    }
    if (!ret)
        FIXME("Mixer channel %s was found for %s, but no channels were found? Wrong selection!\n", snd_mixer_selem_get_name(elem), (snd_mixer_selem_has_playback_volume(elem) ? "playback" : "capture"));
    return ret;
}

static void ALSA_MixerInit(void)
{
    int x, mixnum = 0;

    for (x = 0; x < MAX_MIXERS; ++x)
    {
        int card, err, y;
        char cardind[6], cardname[10];
        BOOL hascapt=0, hasmast=0;
        line *mline;

        snd_ctl_t *ctl;
        snd_mixer_elem_t *elem, *mastelem = NULL, *captelem = NULL;
        snd_ctl_card_info_t *info = NULL;
        snd_ctl_card_info_alloca(&info);
        mixdev[mixnum].lines = NULL;
        mixdev[mixnum].callback = 0;

        snprintf(cardind, sizeof(cardind), "%d", x);
        card = snd_card_get_index(cardind);
        if (card < 0)
            continue;
        snprintf(cardname, sizeof(cardname), "hw:%d", card);

        err = snd_ctl_open(&ctl, cardname, 0);
        if (err < 0)
        {
            WARN("Cannot open card: %s\n", snd_strerror(err));
            continue;
        }

        err = snd_ctl_card_info(ctl, info);
        if (err < 0)
        {
            WARN("Cannot get card info: %s\n", snd_strerror(err));
            snd_ctl_close(ctl);
            continue;
        }

        MultiByteToWideChar(CP_UNIXCP, 0, snd_ctl_card_info_get_name(info), -1, mixdev[mixnum].mixername, sizeof(mixdev[mixnum].mixername)/sizeof(WCHAR));
        snd_ctl_close(ctl);

        err = snd_mixer_open(&mixdev[mixnum].mix,0);
        if (err < 0)
        {
            WARN("Error occured opening mixer: %s\n", snd_strerror(err));
            continue;
        }

        err = snd_mixer_attach(mixdev[mixnum].mix, cardname);
        if (err < 0)
            goto eclose;

        err = snd_mixer_selem_register(mixdev[mixnum].mix, NULL, NULL);
        if (err < 0)
            goto eclose;

        err = snd_mixer_load(mixdev[mixnum].mix);
        if (err < 0)
            goto eclose;

        mixdev[mixnum].chans = 0;
        mixdev[mixnum].dests = 1; /* Master, Capture will be enabled if needed */

        for (elem = snd_mixer_first_elem(mixdev[mixnum].mix); elem; elem = snd_mixer_elem_next(elem))
            if (!strcasecmp(snd_mixer_selem_get_name(elem), "Master"))
            {
                mastelem = elem;
                ++hasmast;
            }
            else if (!strcasecmp(snd_mixer_selem_get_name(elem), "Capture"))
            {
                captelem = elem;
                ++hascapt;
            }
            else if (!blacklisted(elem))
            {
                if (snd_mixer_selem_has_capture_switch(elem))
                {
                    ++mixdev[mixnum].chans;
                    mixdev[mixnum].dests = 2;
                }
                if (snd_mixer_selem_has_playback_volume(elem))
                    ++mixdev[mixnum].chans;
            }

        /* If there is only 'Capture' and 'Master', this device is not worth it */
        if (!mixdev[mixnum].chans)
        {
            WARN("No channels found, skipping device!\n");
            snd_mixer_close(mixdev[mixnum].mix);
            continue;
        }

        /* If there are no 'Capture' and 'Master', something is wrong */
        if (hasmast != 1 || hascapt != 1)
        {
            if (hasmast != 1)
                FIXME("Should have found 1 channel for 'Master', but instead found %d\n", hasmast);
            if (hascapt != 1)
                FIXME("Should have found 1 channel for 'Capture', but instead found %d\n", hascapt);
            goto eclose;
        }

        mixdev[mixnum].chans += 2; /* Capture/Master */
        mixdev[mixnum].lines = calloc(sizeof(MIXERLINEW), mixdev[mixnum].chans);
        err = -ENOMEM;
        if (!mixdev[mixnum].lines)
            goto eclose;

        /* Master control */
        mline = &mixdev[mixnum].lines[0];
        MultiByteToWideChar(CP_UNIXCP, 0, snd_mixer_selem_get_name(mastelem), -1, mline->name, sizeof(mline->name)/sizeof(WCHAR));
        mline->component = getcomponenttype("Master");
        mline->dst = 0;
        mline->capt = 0;
        mline->elem = mastelem;
        mline->chans = chans(&mixdev[mixnum], mastelem, 0);

        /* Capture control
         * Note: since mmixer->dests = 1, it means only playback control is visible
         * This makes sense, because if there are no capture sources capture control
         * can't do anything and should be invisible */

        mline++;
        MultiByteToWideChar(CP_UNIXCP, 0, snd_mixer_selem_get_name(captelem), -1, mline->name, sizeof(mline->name)/sizeof(WCHAR));
        mline->component = getcomponenttype("Capture");
        mline->dst = 1;
        mline->capt = 1;
        mline->elem = captelem;
        mline->chans = chans(&mixdev[mixnum], captelem, 1);

        snd_mixer_elem_set_callback(mastelem, &elem_callback);
        snd_mixer_elem_set_callback_private(mastelem, &mixdev[mixnum]);

        if (mixdev[mixnum].dests == 2)
        {
            snd_mixer_elem_set_callback(captelem, &elem_callback);
            snd_mixer_elem_set_callback_private(captelem, &mixdev[mixnum]);
        }

        y=2;
        for (elem = snd_mixer_first_elem(mixdev[mixnum].mix); elem; elem = snd_mixer_elem_next(elem))
            if (elem != mastelem && elem != captelem && !blacklisted(elem))
            {
                const char * name = snd_mixer_selem_get_name(elem);
                DWORD comp = getcomponenttype(name);
                snd_mixer_elem_set_callback(elem, &elem_callback);
                snd_mixer_elem_set_callback_private(elem, &mixdev[mixnum]);

                if (snd_mixer_selem_has_playback_volume(elem))
                {
                    mline = &mixdev[mixnum].lines[y++];
                    mline->component = comp;
                    MultiByteToWideChar(CP_UNIXCP, 0, name, -1, mline->name, MAXPNAMELEN);
                    mline->capt = mline->dst = 0;
                    mline->elem = elem;
                    mline->chans = chans(&mixdev[mixnum], elem, 0);
                }
                if (snd_mixer_selem_has_capture_switch(elem))
                {
                    mline = &mixdev[mixnum].lines[y++];
                    mline->component = comp;
                    MultiByteToWideChar(CP_UNIXCP, 0, name, -1, mline->name, MAXPNAMELEN);
                    mline->capt = mline->dst = 1;
                    mline->elem = elem;
                    mline->chans = chans(&mixdev[mixnum], elem, 1);
                }
            }

        TRACE("%s: Amount of controls: %i/%i, name: %s\n", cardname, mixdev[mixnum].dests, mixdev[mixnum].chans, debugstr_w(mixdev[mixnum].mixername));
        mixnum++;
        continue;

        eclose:
        WARN("Error occured initialising mixer: %s\n", snd_strerror(err));
        if (mixdev[mixnum].lines)
            free(mixdev[mixnum].lines);
        snd_mixer_close(mixdev[mixnum].mix);
    }
    cards = mixnum;

    InitializeCriticalSection(&elem_crst);
    elem_crst.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": ALSA_MIXER.elem_crst");
    TRACE("\n");
}

static void ALSA_MixerExit(void)
{
    int x;

    if (refcnt)
    {
        WARN("Callback thread still alive, terminating uncleanly, refcnt: %d\n", refcnt);
        /* Least we can do is making sure we're not in 'foreign' code */
        EnterCriticalSection(&elem_crst);
        TerminateThread(thread, 1);
        refcnt = 0;
    }

    TRACE("Cleaning up\n");

    elem_crst.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection(&elem_crst);
    for (x = 0; x < cards; ++x)
    {
        snd_mixer_close(mixdev[x].mix);
        free(mixdev[x].lines);
    }
    cards = 0;
}

static mixer* MIX_GetMix(UINT wDevID)
{
    mixer *mmixer;

    if (wDevID < 0 || wDevID >= cards)
    {
        WARN("Invalid mixer id: %d\n", wDevID);
        return NULL;
    }

    mmixer = &mixdev[wDevID];
    return mmixer;
}

/* Since alsa doesn't tell what exactly changed, just assume all affected controls changed */
static int elem_callback(snd_mixer_elem_t *elem, unsigned int type)
{
    mixer *mmixer = snd_mixer_elem_get_callback_private(elem);
    int x;

    if (type != SND_CTL_EVENT_MASK_VALUE)
        return 0;

    assert(mmixer);

    EnterCriticalSection(&elem_crst);

    if (!mmixer->callback)
        goto out;

    for (x=0; x<mmixer->chans; ++x)
    {
        if (elem != mmixer->lines[x].elem)
            continue;

        TRACE("Found changed control %s\n", debugstr_w(mmixer->lines[x].name));
        mmixer->callback(mmixer->hmx, MM_MIXM_LINE_CHANGE, mmixer->callbackpriv, x, 0);
    }

    out:
    LeaveCriticalSection(&elem_crst);

    return 0;
}

static DWORD WINAPI ALSA_MixerPollThread(LPVOID lParam)
{
    struct pollfd *pfds = NULL;
    int x, y, err, mcnt, count = 1;

    TRACE("%p\n", lParam);

    for (x = 0; x < cards; ++x)
        count += snd_mixer_poll_descriptors_count(mixdev[x].mix);

    TRACE("Counted %d descriptors\n", count);
    pfds = HeapAlloc(GetProcessHeap(), 0, count * sizeof(struct pollfd));

    if (!pfds)
    {
        WARN("Out of memory\n");
        goto die;
    }

    pfds[0].fd = msg_pipe[0];
    pfds[0].events = POLLIN;

    y = 1;
    for (x = 0; x < cards; ++x)
        y += snd_mixer_poll_descriptors(mixdev[x].mix, &pfds[y], count - y);

    while ((err = poll(pfds, (unsigned int) count, -1)) >= 0 || errno == EINTR || errno == EAGAIN)
    {
        if (pfds[0].revents & POLLIN)
            break;

        mcnt = 1;
        for (x = y = 0; x < cards; ++x)
        {
            int j, max = snd_mixer_poll_descriptors_count(mixdev[x].mix);
            for (j = 0; j < max; ++j)
                if (pfds[mcnt+j].revents)
                {
                    y += snd_mixer_handle_events(mixdev[x].mix);
                    break;
                }
            mcnt += max;
        }
        if (y)
            TRACE("Handled %d events\n", y);
    }

    die:
    TRACE("Shutting down\n");
    if (pfds)
        HeapFree(GetProcessHeap(), 0, pfds);

    y = read(msg_pipe[0], &x, sizeof(x));
    close(msg_pipe[1]);
    close(msg_pipe[0]);
    return 0;
}

static DWORD MIX_Open(UINT wDevID, LPMIXEROPENDESC desc, DWORD_PTR flags)
{
    mixer *mmixer = MIX_GetMix(wDevID);
    if (!mmixer)
        return MMSYSERR_BADDEVICEID;

    flags &= CALLBACK_TYPEMASK;
    switch (flags)
    {
    case CALLBACK_NULL:
        goto done;

    case CALLBACK_FUNCTION:
        break;

    default:
        FIXME("Unhandled callback type: %08lx\n", flags & CALLBACK_TYPEMASK);
        return MIXERR_INVALVALUE;
    }

    mmixer->callback = (LPDRVCALLBACK)desc->dwCallback;
    mmixer->callbackpriv = desc->dwInstance;
    mmixer->hmx = (HDRVR)desc->hmx;

    done:
    if (InterlockedIncrement(&refcnt) == 1)
    {
        if (pipe(msg_pipe) >= 0)
        {
            thread = CreateThread(NULL, 0, ALSA_MixerPollThread, NULL, 0, NULL);
            if (!thread)
            {
                close(msg_pipe[0]);
                close(msg_pipe[1]);
                msg_pipe[0] = msg_pipe[1] = -1;
            }
        }
        else
            msg_pipe[0] = msg_pipe[1] = -1;
    }

    return MMSYSERR_NOERROR;
}

static DWORD MIX_Close(UINT wDevID)
{
    int x;
    mixer *mmixer = MIX_GetMix(wDevID);
    if (!mmixer)
        return MMSYSERR_BADDEVICEID;

    EnterCriticalSection(&elem_crst);
    mmixer->callback = 0;
    LeaveCriticalSection(&elem_crst);

    if (!InterlockedDecrement(&refcnt))
    {
        if (write(msg_pipe[1], &x, sizeof(x)) > 0)
        {
            TRACE("Shutting down thread...\n");
            WaitForSingleObject(thread, INFINITE);
            TRACE("Done\n");
        }
    }

    return MMSYSERR_NOERROR;
}

static DWORD MIX_GetDevCaps(UINT wDevID, LPMIXERCAPS2W caps, DWORD_PTR parm2)
{
    mixer *mmixer = MIX_GetMix(wDevID);
    MIXERCAPS2W capsW;

    if (!caps)
        return MMSYSERR_INVALPARAM;

    if (!mmixer)
        return MMSYSERR_BADDEVICEID;

    memset(&capsW, 0, sizeof(MIXERCAPS2W));

    capsW.wMid = WINE_MIXER_MANUF_ID;
    capsW.wPid = WINE_MIXER_PRODUCT_ID;
    capsW.vDriverVersion = WINE_MIXER_VERSION;

    lstrcpynW(capsW.szPname, mmixer->mixername, sizeof(capsW.szPname)/sizeof(WCHAR));
    capsW.cDestinations = mmixer->dests;
    memcpy(caps, &capsW, min(parm2, sizeof(capsW)));
    return MMSYSERR_NOERROR;
}

#endif /*HAVE_ALSA*/

/**************************************************************************
 *                        mxdMessage (WINEALSA.3)
 */
DWORD WINAPI ALSA_mxdMessage(UINT wDevID, UINT wMsg, DWORD_PTR dwUser,
                             DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
#ifdef HAVE_ALSA
    DWORD ret;
    TRACE("(%04X, %s, %08lX, %08lX, %08lX);\n", wDevID, getMessage(wMsg),
          dwUser, dwParam1, dwParam2);

    switch (wMsg)
    {
    case DRVM_INIT: ALSA_MixerInit(); ret = MMSYSERR_NOERROR; break;
    case DRVM_EXIT: ALSA_MixerExit(); ret = MMSYSERR_NOERROR; break;
    /* All taken care of by driver initialisation */
    /* Unimplemented, and not needed */
    case DRVM_ENABLE:
    case DRVM_DISABLE:
        ret = MMSYSERR_NOERROR; break;

    case MXDM_OPEN:
        ret = MIX_Open(wDevID, (LPMIXEROPENDESC) dwParam1, dwParam2); break;

    case MXDM_CLOSE:
        ret = MIX_Close(wDevID); break;

    case MXDM_GETDEVCAPS:
        ret = MIX_GetDevCaps(wDevID, (LPMIXERCAPS2W)dwParam1, dwParam2); break;

    case MXDM_GETNUMDEVS:
        ret = cards; break;

    default:
        WARN("unknown message %s!\n", getMessage(wMsg));
        return MMSYSERR_NOTSUPPORTED;
    }

    TRACE("Returning %08X\n", ret);
    return ret;
#else /*HAVE_ALSA*/
    TRACE("(%04X, %04X, %08lX, %08lX, %08lX);\n", wDevID, wMsg, dwUser, dwParam1, dwParam2);

    return MMSYSERR_NOTENABLED;
#endif /*HAVE_ALSA*/
}
