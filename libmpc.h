#ifndef XMMS_MUSEPACK
#define XMMS_MUSEPACK

//xmms headers
extern "C"
{
#include <audacious/configdb.h>
#include <audacious/plugin.h>
#include <audacious/i18n.h>
#include <audacious/debug.h>
#include <libaudgui/libaudgui.h>
#include <libaudgui/libaudgui-gtk.h>
}

//stdlib headers
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>

//libmpcdec headers
#include <mpc/mpcdec.h>

//GTK+ headers
#include <glib.h>
#include <gtk/gtk.h>

//taglib headers
#include <taglib/tag.h>
#include <taglib/apetag.h>
#include <taglib/mpcfile.h>

struct PluginConfig
{
    gboolean clipPrevention;
    gboolean dynamicBitrate;
    gboolean replaygain;
    gboolean albumGain;
};

struct Widgets
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

struct MpcDecoder
{
    gchar*     isError;
	unsigned   dynbitrate;
    long long  offset;
    bool       isAlive;
    bool       isPause;
};

struct MpcInfo
{
    gchar*    title;
    gchar*    artist;
    gchar*    album;
    gchar*    comment;
    gchar*    genre;
    gchar*    date;
    unsigned  track;
    unsigned  year;
};

static void       toggleSwitch(GtkWidget*, gpointer);
static void       saveConfigBox(GtkWidget*, gpointer);
static void       closeInfoBox(GtkWidget*, gpointer);
static void*      decodeStream(InputPlayback*);

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
