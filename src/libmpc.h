/*
 * Copyright (c) 2005, The Musepack Development Team
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 * 
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 * 
 *     * Neither the name of the The Musepack Development Team nor the
 *       names of its contributors may be used to endorse or promote
 *       products derived from this software without specific prior
 *       written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef XMMS_MUSEPACK
#define XMMS_MUSEPACK

//xmms headers
extern "C"
{
#include <bmp/plugin.h>
#include <bmp/util.h>
#include <bmp/configdb.h>
#include <bmp/titlestring.h>
}

//stdlib headers
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>

//libmpcdec headers
#include <mpcdec/mpcdec.h>

//GTK+ headers
#include <glib.h>
#include <gtk/gtk.h>

//taglib headers
#include <taglib/tag.h>
#include <taglib/apetag.h>
#include <taglib/mpcfile.h>

#include "equalizer.h"

#ifndef M_LN10
#define M_LN10    2.3025850929940456840179914546843642
#endif

typedef struct PluginConfig
{
    gboolean clipPrevention;
    gboolean dynamicBitrate;
    gboolean replaygain;
    gboolean albumGain;
    gboolean isEq;
};

typedef struct Widgets
{
    GtkWidget* aboutBox;
    GtkWidget* configBox;
    GtkWidget* bitrateCheck;
    GtkWidget* clippingCheck;
    GtkWidget* replaygainCheck;
    GtkWidget* albumCheck;
    GtkWidget* infoBox;
    GtkWidget* albumEntry;
    GtkWidget* artistEntry;
    GtkWidget* titleEntry;
    GtkWidget* genreEntry;
    GtkWidget* yearEntry;
    GtkWidget* trackEntry;
    GtkWidget* commentEntry;
    GtkWidget* fileEntry;
};

typedef struct MpcDecoder
{
    char*      isError;
    double     offset;
    bool       isOutput;
    bool       isAlive;
    bool       isPause;
};

typedef struct TrackInfo
{
    int   bitrate;
    char* display;
    int   length;
    int   sampleFreq;
    int   channels;
};

typedef struct MpcInfo
{
    char*    title;
    char*    artist;
    char*    album;
    char*    comment;
    char*    genre;
    char*    date;
    unsigned track;
    unsigned year;
};

extern "C" InputPlugin * get_iplugin_info(void);

static void       mpcOpenPlugin();
static void       mpcAboutBox();
static void       mpcConfigBox();
static void       toggleSwitch(GtkWidget*, gpointer);
static void       saveConfigBox(GtkWidget*, gpointer);
static int        mpcIsOurFile(char*);
static void       mpcPlay(char*);
static void       mpcStop();
static void       mpcPause(short);
static void       mpcSeek(int);
static void       mpcSetEq(int, float, float*);
static int        mpcGetTime();
static void       mpcClosePlugin();
static void       mpcGetSongInfo(char*, char**, int*);
static void       freeTags(MpcInfo&);
static MpcInfo    getTags(const char*);
static void       mpcFileInfoBox(char*);
static void       mpcGtkPrintLabel(GtkWidget*, char*, ...);
static GtkWidget* mpcGtkTagLabel(char*, int, int, int, int, GtkWidget*);
static GtkWidget* mpcGtkTagEntry(int, int, int, int, int, GtkWidget*);
static GtkWidget* mpcGtkLabel(GtkWidget*);
static GtkWidget* mpcGtkButton(char*, GtkWidget*);
static void       removeTags(GtkWidget*, gpointer);
static void       saveTags(GtkWidget*, gpointer);
static void       closeInfoBox(GtkWidget*, gpointer);
static char*      mpcGenerateTitle(const MpcInfo&, const char*);
static void       lockAcquire();
static void       lockRelease();
static void*      decodeStream(void*);
static int        processBuffer(MPC_SAMPLE_FORMAT*, char*, mpc_decoder&);
static void*      endThread(char*, FILE*, bool);
static bool       isAlive();
static void       setAlive(bool);
static double     getOffset();
static void       setOffset(double);
static bool       isPause();
static void       setReplaygain(mpc_streaminfo&, mpc_decoder&);

#ifdef MPC_FIXED_POINT
inline static int shiftSigned(MPC_SAMPLE_FORMAT val, int shift)
{
    if (shift > 0)
        val <<= shift;
    else if (shift < 0)
        val >>= -shift;
    return (int) val;
}
#endif

inline static void copyBuffer(MPC_SAMPLE_FORMAT* pInBuf, char* pOutBuf, unsigned pLength)
{
    unsigned pSize = 16;
    int clipMin    = -1 << (pSize - 1);
    int clipMax    = (1 << (pSize - 1)) - 1;
    int floatScale =  1 << (pSize - 1);
    for (unsigned n = 0; n < 2 * pLength; n++)
    {
        int val;
#ifdef MPC_FIXED_POINT
        val = shiftSigned(pInBuf[n], pSize - MPC_FIXED_POINT_SCALE_SHIFT);
#else
        val = (int) (pInBuf[n] * floatScale);
#endif
        if (val < clipMin)
            val = clipMin;
        else if (val > clipMax)
            val = clipMax;
        unsigned shift = 0;
        do
        {
            pOutBuf[n * 2 + (shift / 8)] = (unsigned char) ((val >> shift) & 0xFF);
            shift += 8;
        }
        while (shift < pSize);
    }
}

#endif
