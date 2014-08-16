#include <string.h>
#include <stdlib.h>
#include <musepack/musepack.h>
extern "C" {
#include "bmp/plugin.h"
#include "bmp/util.h"
#include "bmp/configdb.h"
#include "bmp/titlestring.h"
#include "bmp/vfs.h"
}
#include <glib.h>
#include <gtk/gtk.h>
#include <pthread.h>
#include <math.h>
#include "tags.h"
#include "equalizer.h"
#ifndef M_LN10
#define M_LN10   2.3025850929940456840179914546843642
#endif

// Our mpc_reader / data implementation    
typedef struct reader_data {
    VFSFile *file;
    long size;
    bool seekable;
} reader_data;

mpc_int32_t
read_impl(void *data, void *ptr, mpc_int32_t size)
{
    reader_data *d = (reader_data *) data;

    return vfs_fread(ptr, 1, size, d->file);
}

BOOL
seek_impl(void *data, int offset)
{
    reader_data *d = (reader_data *) data;

    return d->seekable ? !vfs_fseek(d->file, offset, SEEK_SET) : FALSE;
}

mpc_int32_t
tell_impl(void *data)
{
    reader_data *d = (reader_data *) data;

    return vfs_ftell(d->file);
}

mpc_int32_t
get_size_impl(void *data)
{
    reader_data *d = (reader_data *) data;

    return d->size;
}

BOOL
canseek_impl(void *data)
{
    reader_data *d = (reader_data *) data;

    return d->seekable;
}

void
setup_reader(mpc_reader * reader, reader_data * data, VFSFile * input,
                 BOOL seekable)
{
    data->file = input;
    data->seekable = seekable;
    vfs_fseek(data->file, 0, SEEK_END);
    data->size = vfs_ftell(data->file);
    vfs_fseek(data->file, 0, SEEK_SET);

    reader->data = data;
    reader->read = read_impl;
    reader->seek = seek_impl;
    reader->tell = tell_impl;
    reader->get_size = get_size_impl;
    reader->canseek = canseek_impl;

}

extern "C" InputPlugin * get_iplugin_info(void);
static void mpc_load_config();
static void mpc_configure();
static int mpc_is_our_file(char *);
static void mpc_play(char *);
static void mpc_stop(void);
static void mpc_pause(short);
static void mpc_seek(int);
static void mpc_set_eq(int, float, float *);
static int mpc_get_time(void);
static void mpc_get_song_info(char *, char **, int *);
static void mpc_about_box(void);
static void mpc_file_info_box(char *);
static char *generate_title(char *);
static double isSeek;
static short paused;
static gboolean clipPreventionEnabled;
static gboolean replaygainEnabled;
static gboolean dynBitrateEnabled;
static gboolean albumReplaygainEnabled;
static gboolean openedAudio;
static bool AudioError;
static bool killDecodeThread;
static pthread_t thread_handle;
static gboolean EQ_on;


InputPlugin mod = {
    NULL,                       //handle
    NULL,                       //filename
    NULL,
    mpc_load_config,
    mpc_about_box,
    mpc_configure,
    mpc_is_our_file,
    NULL,                       //no use
    mpc_play,
    mpc_stop,
    mpc_pause,
    mpc_seek,
    mpc_set_eq,                 //set eq
    mpc_get_time,
    NULL,                       //get volume
    NULL,                       //set volume
    NULL,                       //cleanup
    NULL,                       //obsolete
    NULL,                       //add_vis
    NULL,
    NULL,
    mpc_get_song_info,
    mpc_file_info_box,          //info box
    NULL,                       //output
};

extern "C" InputPlugin *
get_iplugin_info(void)
{
    mod.description =
        g_strdup_printf(("Musepack Decoder Plugin %s"), VERSION);
    return &mod;
}

static int
mpc_is_our_file(char *filename)
{
    char *ext;
    ext = strrchr(filename, '.');
    if (ext)
        if (!strcasecmp(ext, ".mpc") || !strcasecmp(ext, ".mpp")
            || !strcasecmp(ext, ".mp+"))
            return TRUE;
    return FALSE;
}

#ifdef MPC_FIXED_POINT
static int
shift_signed(MPC_SAMPLE_FORMAT val, int shift)
{
    if (shift > 0)
        val <<= shift;
    else if (shift < 0)
        val >>= -shift;
    return (int)val;
}
#endif

static void
convertLE32to16(MPC_SAMPLE_FORMAT * sample_buffer, char *xmms_buffer,
                unsigned int status)
{
    unsigned int m_bps = 16;    //output on 16 bits
    unsigned n;
    int clip_min = -1 << (m_bps - 1),
        clip_max = (1 << (m_bps - 1)) - 1, float_scale = 1 << (m_bps - 1);
    for (n = 0; n < 2 * status; n++) {
        int val;
#ifdef MPC_FIXED_POINT
        val =
            shift_signed(sample_buffer[n],
                         m_bps - MPC_FIXED_POINT_SCALE_SHIFT);
#else
        val = (int)(sample_buffer[n] * float_scale);
#endif
        if (val < clip_min)
            val = clip_min;
        else if (val > clip_max)
            val = clip_max;
        unsigned shift = 0;
        do {
            xmms_buffer[n * 2 + (shift / 8)] =
                (unsigned char)((val >> shift) & 0xFF);
            shift += 8;
        } while (shift < m_bps);
    }
}


static void
mpc_set_eq(int on, float preamp_ctrl, float *eq_ctrl)
{
    EQ_on = on;
    init_iir(on, preamp_ctrl, eq_ctrl);
}


static int
ReadTag(VFSFile * input, ape_tag * tag)
{
    int res = GetTageType(input);
    *(tag->title) = '\0';
    *(tag->artist) = '\0';
    *(tag->album) = '\0';
    *(tag->comment) = '\0';
    *(tag->genre) = '\0';
    *(tag->track) = '\0';
    *(tag->year) = '\0';

    if (res == TAG_APE) {
        ReadAPE2Tag(input, tag);
    }
    if (res == TAG_ID3) {
        ReadID3Tag(input, tag);
    }
    return res;
}

static void *
end_thread(VFSFile * input)
{
    if (input != NULL) {
        vfs_fclose(input);
        input = NULL;
    }
    pthread_exit(NULL);
    return 0;
}

static void *
DecodeThread(void *a)
{
    ape_tag tag;
    mpc_streaminfo info;
    char *filename = (char *)a;
    int bps_updateCounter = 0;

    VFSFile *input = vfs_fopen(filename, "rb");
    if (input == 0) {
        printf("MPC: Error opening file: \"%s\"\n", filename);
        killDecodeThread = true;
        return end_thread(input);
    }

    mpc_reader reader;
    reader_data data;
    setup_reader(&reader, &data, input, TRUE);
    if (mpc_streaminfo_read(&info, &reader) != ERROR_CODE_OK) {
        printf("MPC: Stream isn't a valid musepack file\n");
        killDecodeThread = true;
        return end_thread(input);
    }

    unsigned short Peak =
        albumReplaygainEnabled ? info.peak_album : info.peak_title;
    short Gain =
        albumReplaygainEnabled ? info.gain_album : info.gain_title;

    double clipPrevFactor = 32767. / (Peak + 1.);   // avoid divide by 0
    double replayGain = exp((M_LN10 / 2000.) * Gain);
    bool noReplaygain = (info.peak_title == 32767.);

    ReadTag(input, &tag);

    mpc_streaminfo_read(&info, &reader);
    mpc_decoder decoder;
    mpc_decoder_setup(&decoder, &reader);
    if (!mpc_decoder_initialize(&decoder, &info)) {
        printf("MPC: Error initializing decoder.\n");
        killDecodeThread = true;
        return end_thread(input);
    }
    if ((!replaygainEnabled || (clipPrevFactor < replayGain))
        && clipPreventionEnabled) {
        mpc_decoder_scale_output(&decoder, clipPrevFactor);
    }
    else if (!noReplaygain && replaygainEnabled) {
        mpc_decoder_scale_output(&decoder, replayGain);
    }

    MPC_SAMPLE_FORMAT sample_buffer[MPC_DECODER_BUFFER_LENGTH];
    gchar xmms_buffer[MPC_DECODER_BUFFER_LENGTH * 4];
    if (!mod.output->
        open_audio(FMT_S16_NE, (int)info.sample_freq,
                   (int)info.channels)) {
        killDecodeThread = true;
        openedAudio = false;
        AudioError = true;
    }
    else {
        mod.set_info(generate_title(filename),
                     (int)(1000 * mpc_streaminfo_get_length(&info)),
                     (int)info.average_bitrate,
                     (int)info.sample_freq, info.channels);
        openedAudio = true;
    }
    unsigned status;
    char *display = generate_title(filename);
    int length = (int)(1000 * mpc_streaminfo_get_length(&info));
    int sampleFreq = (int)info.sample_freq;
    int channels = info.channels;

    // --

    while (!killDecodeThread) {
        mpc_uint32_t vbr_acc = 0;
        mpc_uint32_t vbr_upd = 0;
        if (isSeek != -1) {
            mpc_decoder_seek_seconds(&decoder, isSeek);
            isSeek = -1;
        }
        if (paused == 0
            && (mod.output->buffer_free() >=
                (1152 * 2 *
                 (16 / 8)) << (mod.output->buffer_playing()? 1 : 0))) {
            status = mpc_decoder_decode(&decoder, sample_buffer, &vbr_acc, &vbr_upd);
            if (status == (unsigned)(-1)) {
                printf("MPC: Error decoding file.\n");
                killDecodeThread = true;
                return end_thread(input);
            }
            else if (status == 0) {
                killDecodeThread = true;
                return end_thread(input);
            }
            else {
                convertLE32to16(sample_buffer, xmms_buffer, status);

                if (dynBitrateEnabled) {
                    bps_updateCounter++;
                    if (bps_updateCounter > 20) {
                        mod.set_info(display, length,
                                     1000 * (int)((vbr_upd) * sampleFreq /
                                                  1152.e3), sampleFreq,
                                     channels);
                        bps_updateCounter = 0;
                    }
                }
                if (EQ_on) {
                    iir(xmms_buffer, 4 * status);
                }
                mod.add_vis_pcm(mod.output->written_time(), FMT_S16_LE,
                                info.channels, status * 4,
                                xmms_buffer);
                mod.output->write_audio(xmms_buffer, 4 * status);
            }
        }
        else {
            xmms_usleep(10000);
        }
    }
    killDecodeThread = true;
    return end_thread(input);
}

static void
mpc_play(char *filename)
{
    paused = 0;
    isSeek = -1;
    AudioError = false;
    killDecodeThread = false;
    pthread_create(&thread_handle, NULL, DecodeThread, strdup(filename));
    return;
}

static char *
generate_title(char *fn)
{
    char *displaytitle = NULL;
    VFSFile *input2;
    ape_tag tag2;
    TitleInput *input3;

    input3 = (TitleInput *) g_malloc0(sizeof(TitleInput));
    input3->__size = XMMS_TITLEINPUT_SIZE;
    input3->__version = XMMS_TITLEINPUT_VERSION;

    input2 = vfs_fopen(fn, "rb");
    if (input2 == 0) {
        printf("MPC: Error opening file: \"%s\"\n", fn);
        return '\0';
    }

    int tagtype = ReadTag(input2, &tag2);

    input3->file_name = g_strdup(g_basename(fn));
    input3->file_ext = "mpc";

    input3->track_name = g_strdup(tag2.title);
    input3->performer = g_strdup(tag2.artist);
    input3->album_name = g_strdup(tag2.album);
    input3->date = g_strdup(tag2.year);
    input3->track_number = atoi(tag2.track);
    if (input3->track_number < 0)
        input3->track_number = 0;
    input3->year = atoi(tag2.year);
    if (input3->year < 0)
        input3->year = 0;
    input3->genre = g_strdup(tag2.genre);
    input3->comment = g_strdup(tag2.comment);

    if (tagtype != TAG_NONE) {
        displaytitle =
            xmms_get_titlestring(xmms_get_gentitle_format(), input3);
    }
    else {
        displaytitle = (char *)malloc(strlen(input3->file_name) - 3);
        displaytitle[strlen(input3->file_name) - 4] = '\0';
        displaytitle =
            strncpy(displaytitle, input3->file_name,
                    strlen(input3->file_name) - 4);
    }
    g_free(input3->track_name);
    g_free(input3->performer);
    g_free(input3->album_name);
    g_free(input3->genre);
    g_free(input3->comment);
    g_free(input3);

    if (input2 != NULL)
        vfs_fclose(input2);
    return displaytitle;
}

static void
mpc_get_song_info(char *filename, char **title, int *length)
{
    mpc_streaminfo info;
    VFSFile *input;
    input = vfs_fopen(filename, "rb");
    if (input == 0) {
        printf("MPC: Error opening file: \"%s\"\n", filename);
        return;
    }

    reader_data data;
    mpc_reader reader;
    setup_reader(&reader, &data, input, TRUE);
    mpc_streaminfo_read(&info, &reader);
    *length = 1000 * (int)mpc_streaminfo_get_length(&info);
    if (input != NULL)
        vfs_fclose(input);
    *title = generate_title(filename);
}

static int
mpc_get_time(void)
{
    if (!mod.output)
        return -1;
    if (AudioError)
        return -2;
    if (killDecodeThread && !mod.output->buffer_playing())
        return -1;
    return mod.output->output_time();
}


static void
mpc_seek(int sec)
{
    isSeek = sec;
    mod.output->flush((int)(1000 * isSeek));
}

static void
mpc_pause(short pause)
{
    mod.output->pause(paused = pause);
}

static void
mpc_stop(void)
{
    killDecodeThread = true;
    if (thread_handle != 0) {
        pthread_join(thread_handle, NULL);


        if (openedAudio) {
            mod.output->buffer_free();
            mod.output->close_audio();
            openedAudio = false;
        }
        if (AudioError) {
            /*xmms_show_message(g_strdup_printf("Musepack Decoder Plugin %s",VERSION),g_locale_to_utf8(
               "Could not open Audio",-1,NULL,NULL,NULL),
               ("Ok"), FALSE, NULL, NULL); */
            printf("Could not open Audio\n");
            AudioError = false;
        }
    }
}



/*##############################################################################
################################################################################
##############################################################################*/

static GtkWidget *window = NULL;
static GtkWidget *title_entry;
static GtkWidget *album_entry;
static GtkWidget *performer_entry;
static GtkWidget *tracknumber_entry;
static GtkWidget *date_entry;
static GtkWidget *genre_entry;
static GtkWidget *user_comment_entry;
static GtkWidget *filename_entry;
static gchar *filename;

static void
mpc_about_box()
{
    static GtkWidget *about_window;

    if (about_window)
        gdk_window_raise(about_window->window);

    about_window =
        xmms_show_message(g_strdup_printf
                          ("Musepack Decoder Plugin %s", VERSION),
                          g_locale_to_utf8("Plugin code by \n"
                                           "Benoit Amiaux\n"
                                           "Martin Spüler\n\n"
                                           "Musepack code by\n"
                                           "Andree Buschmann\n"
                                           "Frank Klemm\n"
                                           "Peter Pawlowski\n\n"
                                           "Visit the Musepack site at http://www.musepack.net/\n",
                                           -1, NULL, NULL, NULL), ("Ok"),
                          FALSE, NULL, NULL);
    gtk_signal_connect(GTK_OBJECT(about_window), "destroy",
                       G_CALLBACK(gtk_widget_destroyed), &about_window);
}

static void
mpc_load_config()
{
    ConfigDb *cfg;
    cfg = bmp_cfg_db_open();

    bmp_cfg_db_get_bool(cfg, "musepack", "clip_prevention",
                        &clipPreventionEnabled);
    bmp_cfg_db_get_bool(cfg, "musepack", "album_replaygain",
                        &albumReplaygainEnabled);
    bmp_cfg_db_get_bool(cfg, "musepack", "replaygain", &replaygainEnabled);
    bmp_cfg_db_get_bool(cfg, "musepack", "dyn_bitrate", &dynBitrateEnabled);

    bmp_cfg_db_close(cfg);
    openedAudio = false;
}

static void
label_set_text(GtkWidget * label, char *str, ...)
{
    va_list args;
    gchar *tempstr;

    va_start(args, str);
    tempstr = g_strdup_vprintf(str, args);
    va_end(args);

    gtk_label_set_text(GTK_LABEL(label), tempstr);
    g_free(tempstr);
}

static void
remove_cb(GtkWidget * w, gpointer data)
{
    DeleteTag(filename);
    g_free(filename);
    gtk_widget_destroy(window);
}

static void
save_cb(GtkWidget * w, gpointer data)
{
    ape_tag Tag;
    strcpy(Tag.title, gtk_entry_get_text(GTK_ENTRY(title_entry)));
    strcpy(Tag.artist, gtk_entry_get_text(GTK_ENTRY(performer_entry)));
    strcpy(Tag.album, gtk_entry_get_text(GTK_ENTRY(album_entry)));
    strcpy(Tag.comment, gtk_entry_get_text(GTK_ENTRY(user_comment_entry)));
    strcpy(Tag.track, gtk_entry_get_text(GTK_ENTRY(tracknumber_entry)));
    strcpy(Tag.year, gtk_entry_get_text(GTK_ENTRY(date_entry)));
    strcpy(Tag.genre, gtk_entry_get_text(GTK_ENTRY(genre_entry)));
    WriteAPE2Tag(filename, &Tag);
    g_free(filename);
    gtk_widget_destroy(window);
}

static void
close_window(GtkWidget * w, gpointer data)
{
    g_free(filename);
    gtk_widget_destroy(window);
}

static void
mpc_file_info_box(gchar * fn)
{
    gchar *tmp;
    gint time, minutes, seconds;
    int tagtype;


    mpc_streaminfo info;
    VFSFile *input = vfs_fopen(fn, "rb");
    if (input == NULL) {
        printf("MPC: Error opening file: \"%s\"\n", fn);
        return;
    }

    filename = (char *)malloc(strlen(fn) + 1);
    strcpy(filename, fn);

    mpc_reader reader;
    reader_data data;
    setup_reader(&reader, &data, input, TRUE);
    mpc_streaminfo_read(&info, &reader);
    ape_tag tag2;
    tagtype = ReadTag(input, &tag2);
    vfs_fclose(input);

    static GtkWidget *info_frame, *info_box, *bitrate_label, *rate_label;
    static GtkWidget *streamversion_label, *encoder_label, *profile_label;
    static GtkWidget *channel_label, *length_label, *filesize_label;
    static GtkWidget *peakTitle_label, *peakAlbum_label, *gainTitle_label;
    static GtkWidget *gainAlbum_label, *tag_frame;
    if (!window) {
        GtkWidget *hbox, *label, *filename_hbox, *vbox, *left_vbox;
        GtkWidget *table, *bbox, *cancel_button;
        GtkWidget *save_button, *remove_button;

        window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_type_hint(GTK_WINDOW(window),
                                 GDK_WINDOW_TYPE_HINT_DIALOG);
        gtk_window_set_policy(GTK_WINDOW(window), FALSE, FALSE, FALSE);
        gtk_signal_connect(GTK_OBJECT(window), "destroy",
                           GTK_SIGNAL_FUNC(gtk_widget_destroyed), &window);
        gtk_container_set_border_width(GTK_CONTAINER(window), 10);

        vbox = gtk_vbox_new(FALSE, 10);
        gtk_container_add(GTK_CONTAINER(window), vbox);

        filename_hbox = gtk_hbox_new(FALSE, 5);
        gtk_box_pack_start(GTK_BOX(vbox), filename_hbox, FALSE, TRUE, 0);

        label = gtk_label_new("Filename:");
        gtk_box_pack_start(GTK_BOX(filename_hbox), label, FALSE, TRUE, 0);
        filename_entry = gtk_entry_new();
        gtk_editable_set_editable(GTK_EDITABLE(filename_entry), FALSE);
        gtk_box_pack_start(GTK_BOX(filename_hbox), filename_entry,
                           TRUE, TRUE, 0);

        hbox = gtk_hbox_new(FALSE, 10);
        gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, TRUE, 0);

        left_vbox = gtk_vbox_new(FALSE, 10);
        gtk_box_pack_start(GTK_BOX(hbox), left_vbox, FALSE, FALSE, 0);

        tag_frame =
            gtk_frame_new((tagtype ==
                           TAG_ID3 ? "Musepack ID3 Tag" :
                           "Musepack Ape2 Tag:"));
        gtk_box_pack_start(GTK_BOX(left_vbox), tag_frame, FALSE, FALSE, 0);

        table = gtk_table_new(5, 5, FALSE);
        gtk_container_set_border_width(GTK_CONTAINER(table), 5);
        gtk_container_add(GTK_CONTAINER(tag_frame), table);

        label = gtk_label_new("Title:");
        gtk_misc_set_alignment(GTK_MISC(label), 1, 0.5);
        gtk_table_attach(GTK_TABLE(table), label, 0, 1, 0, 1,
                         GTK_FILL, GTK_FILL, 5, 5);

        title_entry = gtk_entry_new();
        gtk_table_attach(GTK_TABLE(table), title_entry, 1, 4, 0, 1,
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK),
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK), 0, 5);

        label = gtk_label_new("Artist:");
        gtk_misc_set_alignment(GTK_MISC(label), 1, 0.5);
        gtk_table_attach(GTK_TABLE(table), label, 0, 1, 1, 2,
                         GTK_FILL, GTK_FILL, 5, 5);

        performer_entry = gtk_entry_new();
        gtk_table_attach(GTK_TABLE(table), performer_entry, 1, 4, 1, 2,
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK),
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK), 0, 5);

        label = gtk_label_new("Album:");
        gtk_misc_set_alignment(GTK_MISC(label), 1, 0.5);
        gtk_table_attach(GTK_TABLE(table), label, 0, 1, 2, 3,
                         GTK_FILL, GTK_FILL, 5, 5);

        album_entry = gtk_entry_new();
        gtk_table_attach(GTK_TABLE(table), album_entry, 1, 4, 2, 3,
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK),
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK), 0, 5);

        label = gtk_label_new("Comment:");
        gtk_misc_set_alignment(GTK_MISC(label), 1, 0.5);
        gtk_table_attach(GTK_TABLE(table), label, 0, 1, 3, 4,
                         GTK_FILL, GTK_FILL, 5, 5);

        user_comment_entry = gtk_entry_new();
        gtk_table_attach(GTK_TABLE(table), user_comment_entry, 1, 4, 3,
                         4,
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK),
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK), 0, 5);

        label = gtk_label_new("Year:");
        gtk_misc_set_alignment(GTK_MISC(label), 1, 0.5);
        gtk_table_attach(GTK_TABLE(table), label, 0, 1, 4, 5,
                         GTK_FILL, GTK_FILL, 5, 5);

        date_entry = gtk_entry_new();
        gtk_widget_set_usize(date_entry, 60, -1);
        gtk_table_attach(GTK_TABLE(table), date_entry, 1, 2, 4, 5,
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK),
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK), 0, 5);

        label = gtk_label_new("Track nÂ°:");
        gtk_misc_set_alignment(GTK_MISC(label), 1, 0.5);
        gtk_table_attach(GTK_TABLE(table), label, 2, 3, 4, 5,
                         GTK_FILL, GTK_FILL, 5, 5);

        tracknumber_entry = gtk_entry_new_with_max_length(4);
        gtk_widget_set_usize(tracknumber_entry, 20, -1);
        gtk_table_attach(GTK_TABLE(table), tracknumber_entry, 3, 4, 4,
                         5,
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK),
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK), 0, 5);

        label = gtk_label_new("Genre:");
        gtk_misc_set_alignment(GTK_MISC(label), 1, 0.5);
        gtk_table_attach(GTK_TABLE(table), label, 0, 1, 5, 6,
                         GTK_FILL, GTK_FILL, 5, 5);

        genre_entry = gtk_entry_new();
        gtk_widget_set_usize(genre_entry, 20, -1);
        gtk_table_attach(GTK_TABLE(table), genre_entry, 1, 4, 5,
                         6,
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK),
                         (GtkAttachOptions) (GTK_FILL | GTK_EXPAND |
                                             GTK_SHRINK), 0, 5);

        bbox = gtk_hbutton_box_new();
        gtk_button_box_set_layout(GTK_BUTTON_BOX(bbox), GTK_BUTTONBOX_END);
        gtk_button_box_set_spacing(GTK_BUTTON_BOX(bbox), 5);
        gtk_box_pack_start(GTK_BOX(left_vbox), bbox, FALSE, FALSE, 0);

        save_button = gtk_button_new_with_label("Save");
        gtk_signal_connect(GTK_OBJECT(save_button),
                           "clicked", GTK_SIGNAL_FUNC(save_cb), NULL);
        GTK_WIDGET_SET_FLAGS(save_button, GTK_CAN_DEFAULT);
        gtk_box_pack_start(GTK_BOX(bbox), save_button, TRUE, TRUE, 0);

        remove_button = gtk_button_new_with_label("Remove Tag");
        gtk_signal_connect_object(GTK_OBJECT(remove_button),
                                  "clicked",
                                  GTK_SIGNAL_FUNC(remove_cb), NULL);
        GTK_WIDGET_SET_FLAGS(remove_button, GTK_CAN_DEFAULT);
        gtk_box_pack_start(GTK_BOX(bbox), remove_button, TRUE, TRUE, 0);

        cancel_button = gtk_button_new_with_label("Cancel");
        gtk_signal_connect_object(GTK_OBJECT(cancel_button),
                                  "clicked",
                                  GTK_SIGNAL_FUNC(close_window), NULL);
        GTK_WIDGET_SET_FLAGS(cancel_button, GTK_CAN_DEFAULT);
        gtk_box_pack_start(GTK_BOX(bbox), cancel_button, TRUE, TRUE, 0);
        gtk_widget_grab_default(cancel_button);

        info_frame = gtk_frame_new("Musepack Info:");
        gtk_box_pack_start(GTK_BOX(hbox), info_frame, FALSE, FALSE, 0);

        info_box = gtk_vbox_new(FALSE, 5);
        gtk_container_add(GTK_CONTAINER(info_frame), info_box);
        gtk_container_set_border_width(GTK_CONTAINER(info_box), 10);
        gtk_box_set_spacing(GTK_BOX(info_box), 0);

        streamversion_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(streamversion_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(streamversion_label),
                              GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), streamversion_label, FALSE,
                           FALSE, 0);

        encoder_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(encoder_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(encoder_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), encoder_label, FALSE, FALSE, 0);

        profile_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(profile_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(profile_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), profile_label, FALSE, FALSE, 0);

        bitrate_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(bitrate_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(bitrate_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), bitrate_label, FALSE, FALSE, 0);

        rate_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(rate_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(rate_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), rate_label, FALSE, FALSE, 0);

        channel_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(channel_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(channel_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), channel_label, FALSE, FALSE, 0);

        length_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(length_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(length_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), length_label, FALSE, FALSE, 0);

        filesize_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(filesize_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(filesize_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), filesize_label, FALSE,
                           FALSE, 0);

        peakTitle_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(peakTitle_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(peakTitle_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), peakTitle_label, FALSE,
                           FALSE, 0);

        peakAlbum_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(peakAlbum_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(peakAlbum_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), peakAlbum_label, FALSE,
                           FALSE, 0);

        gainTitle_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(gainTitle_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(gainTitle_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), gainTitle_label, FALSE,
                           FALSE, 0);

        gainAlbum_label = gtk_label_new("");
        gtk_misc_set_alignment(GTK_MISC(gainAlbum_label), 0, 0);
        gtk_label_set_justify(GTK_LABEL(gainAlbum_label), GTK_JUSTIFY_LEFT);
        gtk_box_pack_start(GTK_BOX(info_box), gainAlbum_label, FALSE,
                           FALSE, 0);

        gtk_widget_show_all(window);
    }
    else
        gdk_window_raise(window->window);

    gtk_widget_set_sensitive(tag_frame, TRUE);

    gtk_label_set_text(GTK_LABEL(streamversion_label), "");
    gtk_label_set_text(GTK_LABEL(encoder_label), "");
    gtk_label_set_text(GTK_LABEL(profile_label), "");
    gtk_label_set_text(GTK_LABEL(bitrate_label), "");
    gtk_label_set_text(GTK_LABEL(rate_label), "");
    gtk_label_set_text(GTK_LABEL(channel_label), "");
    gtk_label_set_text(GTK_LABEL(length_label), "");
    gtk_label_set_text(GTK_LABEL(filesize_label), "");
    gtk_label_set_text(GTK_LABEL(peakTitle_label), "");
    gtk_label_set_text(GTK_LABEL(peakAlbum_label), "");
    gtk_label_set_text(GTK_LABEL(gainTitle_label), "");
    gtk_label_set_text(GTK_LABEL(gainAlbum_label), "");

    time = (gint)mpc_streaminfo_get_length(&info);
    minutes = time / 60;
    seconds = time % 60;

    label_set_text(streamversion_label, "Streamversion %d",
                   info.stream_version);
    label_set_text(encoder_label, "Encoder: %s", info.encoder);
    label_set_text(profile_label, "Profile: %s", info.profile_name);
    label_set_text(bitrate_label, "Average bitrate: %6.1f kbps",
                   info.average_bitrate * 1.e-3);
    label_set_text(rate_label, "Samplerate: %d Hz", info.sample_freq);
    label_set_text(channel_label, "Channels: %d", info.channels);
    label_set_text(length_label, "Length: %d:%.2d", minutes, seconds);
    label_set_text(filesize_label, "File size: %d Bytes",
                   info.total_file_length);
    label_set_text(peakTitle_label, "Title Peak: %5u",
                   info.peak_title);
    label_set_text(peakAlbum_label, "Album Peak: %5u",
                   info.peak_album);
    label_set_text(gainTitle_label, "Title Gain: %-+5.2f dB",
                   0.01 * info.gain_title);
    label_set_text(gainAlbum_label, "Album Gain: %-+5.2f dB",
                   0.01 * info.gain_album);

    if (tagtype == TAG_ID3) {
        gtk_entry_set_text(GTK_ENTRY(title_entry),
                           g_locale_to_utf8(tag2.title, -1, NULL, NULL,
                                            NULL));
        gtk_entry_set_text(GTK_ENTRY(performer_entry),
                           g_locale_to_utf8(tag2.artist, -1, NULL, NULL,
                                            NULL));
        gtk_entry_set_text(GTK_ENTRY(album_entry),
                           g_locale_to_utf8(tag2.album, -1, NULL, NULL,
                                            NULL));
        gtk_entry_set_text(GTK_ENTRY(user_comment_entry),
                           g_locale_to_utf8(tag2.comment, -1, NULL, NULL,
                                            NULL));
        gtk_entry_set_text(GTK_ENTRY(genre_entry),
                           g_locale_to_utf8(tag2.genre, -1, NULL, NULL,
                                            NULL));
        gtk_entry_set_text(GTK_ENTRY(tracknumber_entry),
                           g_locale_to_utf8(tag2.track, -1, NULL, NULL,
                                            NULL));
        gtk_entry_set_text(GTK_ENTRY(date_entry),
                           g_locale_to_utf8(tag2.year, -1, NULL, NULL, NULL));
    }
    else {
        gtk_entry_set_text(GTK_ENTRY(title_entry), tag2.title);
        gtk_entry_set_text(GTK_ENTRY(performer_entry), tag2.artist);
        gtk_entry_set_text(GTK_ENTRY(album_entry), tag2.album);
        gtk_entry_set_text(GTK_ENTRY(user_comment_entry), tag2.comment);
        gtk_entry_set_text(GTK_ENTRY(genre_entry), tag2.genre);
        gtk_entry_set_text(GTK_ENTRY(tracknumber_entry), tag2.track);
        gtk_entry_set_text(GTK_ENTRY(date_entry), tag2.year);
    }
    gtk_entry_set_text(GTK_ENTRY(filename_entry),
                       g_filename_to_utf8(fn, -1, NULL, NULL, NULL));
    gtk_editable_set_position(GTK_EDITABLE(filename_entry), -1);

    tmp = g_strdup_printf("File Info - %s", g_basename(fn));
    gtk_window_set_title(GTK_WINDOW(window), tmp);
    g_free(tmp);
}

static GtkWidget *mpc_configurewin = NULL;
static GtkWidget *vbox, *notebook;
static GtkWidget *rg_switch, *rg_clip_switch, *rg_track_gain, *rg_dyn_bitrate;

static void
mpc_configurewin_ok(GtkWidget * widget, gpointer data)
{
    ConfigDb *cfg;
    GtkToggleButton *tb;

    tb = GTK_TOGGLE_BUTTON(rg_switch);
    replaygainEnabled = gtk_toggle_button_get_active(tb);
    tb = GTK_TOGGLE_BUTTON(rg_clip_switch);
    clipPreventionEnabled = gtk_toggle_button_get_active(tb);
    tb = GTK_TOGGLE_BUTTON(rg_dyn_bitrate);
    dynBitrateEnabled = gtk_toggle_button_get_active(tb);
    tb = GTK_TOGGLE_BUTTON(rg_track_gain);
    albumReplaygainEnabled = !gtk_toggle_button_get_active(tb);

    cfg = bmp_cfg_db_open();

    bmp_cfg_db_set_bool(cfg, "musepack", "clip_prevention",
                        clipPreventionEnabled);
    bmp_cfg_db_set_bool(cfg, "musepack", "album_replaygain",
                        albumReplaygainEnabled);
    bmp_cfg_db_set_bool(cfg, "musepack", "dyn_bitrate", dynBitrateEnabled);
    bmp_cfg_db_set_bool(cfg, "musepack", "replaygain", replaygainEnabled);
    bmp_cfg_db_close(cfg);
    gtk_widget_destroy(mpc_configurewin);
}

static void
rg_switch_cb(GtkWidget * w, gpointer data)
{
    gtk_widget_set_sensitive(GTK_WIDGET(data),
                             gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON
                                                          (w)));
}

void
mpc_configure(void)
{

    GtkWidget *rg_frame, *rg_vbox;
    GtkWidget *bbox, *ok, *cancel;
    GtkWidget *rg_type_frame, *rg_type_vbox, *rg_album_gain;

    if (mpc_configurewin != NULL) {
        gdk_window_raise(mpc_configurewin->window);
        return;
    }

    mpc_configurewin = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_type_hint(GTK_WINDOW(mpc_configurewin),
                             GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_signal_connect(GTK_OBJECT(mpc_configurewin), "destroy",
                       GTK_SIGNAL_FUNC(gtk_widget_destroyed),
                       &mpc_configurewin);
    gtk_window_set_title(GTK_WINDOW(mpc_configurewin),
                         "Musepack Configuration");
    gtk_window_set_policy(GTK_WINDOW(mpc_configurewin), FALSE, FALSE, FALSE);
    gtk_container_border_width(GTK_CONTAINER(mpc_configurewin), 10);

    vbox = gtk_vbox_new(FALSE, 10);
    gtk_container_add(GTK_CONTAINER(mpc_configurewin), vbox);

    notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(vbox), notebook, TRUE, TRUE, 0);

    /* Plugin Settings */

    rg_frame = gtk_frame_new("General Plugin Settings:");
    gtk_container_border_width(GTK_CONTAINER(rg_frame), 5);

    rg_vbox = gtk_vbox_new(FALSE, 10);
    gtk_container_border_width(GTK_CONTAINER(rg_vbox), 5);
    gtk_container_add(GTK_CONTAINER(rg_frame), rg_vbox);

    rg_dyn_bitrate =
        gtk_check_button_new_with_label("Enable Dynamic Bitrate Display");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rg_dyn_bitrate),
                                 dynBitrateEnabled);
    gtk_box_pack_start(GTK_BOX(rg_vbox), rg_dyn_bitrate, FALSE, FALSE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), rg_frame,
                             gtk_label_new("Plugin"));

    /* Replay Gain.. */

    rg_frame = gtk_frame_new("ReplayGain Settings:");
    gtk_container_border_width(GTK_CONTAINER(rg_frame), 5);

    rg_vbox = gtk_vbox_new(FALSE, 10);
    gtk_container_border_width(GTK_CONTAINER(rg_vbox), 5);
    gtk_container_add(GTK_CONTAINER(rg_frame), rg_vbox);

    rg_clip_switch =
        gtk_check_button_new_with_label("Enable Clipping Prevention");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rg_clip_switch),
                                 clipPreventionEnabled);
    gtk_box_pack_start(GTK_BOX(rg_vbox), rg_clip_switch, FALSE, FALSE, 0);


    rg_switch = gtk_check_button_new_with_label("Enable ReplayGain");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rg_switch),
                                 replaygainEnabled);
    gtk_box_pack_start(GTK_BOX(rg_vbox), rg_switch, FALSE, FALSE, 0);

    rg_type_frame = gtk_frame_new("ReplayGain Type:");
    gtk_box_pack_start(GTK_BOX(rg_vbox), rg_type_frame, FALSE, FALSE, 0);

    gtk_signal_connect(GTK_OBJECT(rg_switch), "toggled",
                       GTK_SIGNAL_FUNC(rg_switch_cb), rg_type_frame);

    rg_type_vbox = gtk_vbox_new(FALSE, 5);
    gtk_container_set_border_width(GTK_CONTAINER(rg_type_vbox), 5);
    gtk_container_add(GTK_CONTAINER(rg_type_frame), rg_type_vbox);

    rg_track_gain =
        gtk_radio_button_new_with_label(NULL, "use Track Gain/Peak");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rg_track_gain),
                                 !albumReplaygainEnabled);
    gtk_box_pack_start(GTK_BOX(rg_type_vbox), rg_track_gain, FALSE, FALSE, 0);

    rg_album_gain =
        gtk_radio_button_new_with_label(gtk_radio_button_group
                                        (GTK_RADIO_BUTTON(rg_track_gain)),
                                        "use Album Gain/Peak");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rg_album_gain),
                                 albumReplaygainEnabled);
    gtk_box_pack_start(GTK_BOX(rg_type_vbox), rg_album_gain, FALSE, FALSE, 0);

    gtk_widget_set_sensitive(rg_type_frame, replaygainEnabled);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), rg_frame,
                             gtk_label_new("ReplayGain"));

    /* Buttons */

    bbox = gtk_hbutton_box_new();
    gtk_button_box_set_layout(GTK_BUTTON_BOX(bbox), GTK_BUTTONBOX_END);
    gtk_button_box_set_spacing(GTK_BUTTON_BOX(bbox), 5);
    gtk_box_pack_start(GTK_BOX(vbox), bbox, FALSE, FALSE, 0);

    ok = gtk_button_new_with_label("Ok");
    gtk_signal_connect(GTK_OBJECT(ok), "clicked",
                       GTK_SIGNAL_FUNC(mpc_configurewin_ok), NULL);
    GTK_WIDGET_SET_FLAGS(ok, GTK_CAN_DEFAULT);
    gtk_box_pack_start(GTK_BOX(bbox), ok, TRUE, TRUE, 0);
    gtk_widget_grab_default(ok);

    cancel = gtk_button_new_with_label("Cancel");
    gtk_signal_connect_object(GTK_OBJECT(cancel), "clicked",
                              GTK_SIGNAL_FUNC(gtk_widget_destroy),
                              GTK_OBJECT(mpc_configurewin));
    GTK_WIDGET_SET_FLAGS(cancel, GTK_CAN_DEFAULT);
    gtk_box_pack_start(GTK_BOX(bbox), cancel, TRUE, TRUE, 0);

    gtk_widget_show_all(mpc_configurewin);
}
