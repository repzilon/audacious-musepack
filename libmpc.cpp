/*
 * Copyright (c) 2005-2009, The Musepack Development Team
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


#include "libmpc.h"

#define REMOVE_NONEXISTANT_TAG(x)   if (!*x) { x = NULL; }
#define VERSION "1.3"
#define PACKAGE "aud_mpc"

using TagLib::MPC::File;
using TagLib::Tag;
using TagLib::String;
using TagLib::APE::ItemListMap;

static PluginConfig pluginConfig = {0};
static Widgets      widgets      = {0};
static MpcDecoder   mpcDecoder   = {0};
static mpc_streaminfo	streamInfo	= {0};

static GThread            *threadHandle;
static GStaticMutex threadMutex = G_STATIC_MUTEX_INIT;

/*
 * VFS callback implementation, adapted from mpc_reader.c.
 * This _IS_ very sick, but it works. -nenolod
 */
static mpc_int32_t
aud_vfs_fread_impl(mpc_reader *d, void *ptr, mpc_int32_t size)
{
    VFSFile *file = (VFSFile *) d->data;

    return (mpc_int32_t) vfs_fread(ptr, 1, size, file);
}

static mpc_bool_t
aud_vfs_fseek_impl(mpc_reader *d, mpc_int32_t offset)
{
    VFSFile *file = (VFSFile *) d->data;

	return d->canseek(d) ? vfs_fseek(file, offset, SEEK_SET) == 0 : FALSE;
}

static mpc_int32_t
aud_vfs_ftell_impl(mpc_reader *d)
{
    VFSFile *file = (VFSFile *) d->data;

    return vfs_ftell(file);
}

static mpc_int32_t
aud_vfs_getsize_impl(mpc_reader *d)
{
	int f_pos, f_size;
	VFSFile *file = (VFSFile *) d->data;

	f_pos = vfs_ftell(file);
	if (vfs_fseek(file, 0, SEEK_END) != 0) {
        AUDDBG("Could not seek to the end of file\n");
		return 0;
	}
	f_size = vfs_ftell(file);
	if (vfs_fseek(file, f_pos, SEEK_SET) != 0)
        AUDDBG("Could not seek to %d\n", f_pos);

	return f_size;
}

static mpc_bool_t
aud_vfs_canseek_impl(mpc_reader *d)
{
    return TRUE; // FIXME : can we really always seek ?
}

/*
 * This sets up an mpc_reader object to read from VFS instead of libc.
 * Essentially, we use this instead of the normal constructor.
 *    - nenolod
 */
void
mpc_reader_setup_file_vfs(mpc_reader *p_reader, VFSFile *input)
{
    p_reader->seek = aud_vfs_fseek_impl;
    p_reader->read = aud_vfs_fread_impl;
    p_reader->tell = aud_vfs_ftell_impl;
    p_reader->get_size = aud_vfs_getsize_impl;
    p_reader->canseek = aud_vfs_canseek_impl;
	p_reader->data = input; // no worries, it gets cast back -nenolod
	if (vfs_fseek(input, 0, SEEK_SET) != 0)
        AUDDBG("Could not seek to the beginning of file\n");
}

static void mpcOpenPlugin()
{
    mcs_handle_t *cfg;
    cfg = aud_cfg_db_open();
    aud_cfg_db_get_bool(cfg, "musepack", "clipPrevention", &pluginConfig.clipPrevention);
    aud_cfg_db_get_bool(cfg, "musepack", "albumGain",      &pluginConfig.albumGain);
    aud_cfg_db_get_bool(cfg, "musepack", "dynamicBitrate", &pluginConfig.dynamicBitrate);
    aud_cfg_db_get_bool(cfg, "musepack", "replaygain",     &pluginConfig.replaygain);
    aud_cfg_db_close(cfg);
}

static void mpcAboutBox()
{
    GtkWidget* aboutBox = widgets.aboutBox;
    if (aboutBox)
        gdk_window_raise(aboutBox->window);
    else
    {
        char* titleText      = g_strdup_printf(_("Musepack Decoder Plugin %s"), VERSION);
        const char* contentText = _("Plugin code by\nBenoit Amiaux\nMartin Spuler\nKuniklo\nNicolas Botti\n\nGet latest version at http://musepack.net\n");
		audgui_simple_message (& aboutBox, GTK_MESSAGE_INFO, titleText, contentText);
        widgets.aboutBox = aboutBox;
        g_signal_connect(G_OBJECT(aboutBox), "destroy", G_CALLBACK(gtk_widget_destroyed), &widgets.aboutBox);
    }
}

static void toggleSwitch(GtkWidget* p_Widget, gpointer p_Data)
{
    gtk_widget_set_sensitive(GTK_WIDGET(p_Data), gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p_Widget)));
}

static void mpcConfigBox()
{
    GtkWidget* configBox = widgets.configBox;
    if(configBox)
        gdk_window_raise(configBox->window);
    else
    {
        configBox = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_type_hint(GTK_WINDOW(configBox), GDK_WINDOW_TYPE_HINT_DIALOG);
        widgets.configBox = configBox;
        g_signal_connect(G_OBJECT(configBox), "destroy", G_CALLBACK(gtk_widget_destroyed), &widgets.configBox);
        gtk_window_set_title(GTK_WINDOW(configBox), _("Musepack Decoder Configuration"));
        gtk_window_set_resizable(GTK_WINDOW(configBox), FALSE);
        gtk_container_set_border_width(GTK_CONTAINER(configBox), 10);

        GtkWidget* notebook = gtk_notebook_new();
        GtkWidget* vbox = gtk_vbox_new(FALSE, 10);
        gtk_box_pack_start(GTK_BOX(vbox), notebook, TRUE, TRUE, 0);
        gtk_container_add(GTK_CONTAINER(configBox), vbox);

        //General Settings Tab
        GtkWidget* generalSet = gtk_frame_new(_("General Settings"));
        gtk_container_set_border_width(GTK_CONTAINER(generalSet), 5);

        GtkWidget* gSvbox = gtk_vbox_new(FALSE, 10);
        gtk_container_set_border_width(GTK_CONTAINER(gSvbox), 5);
        gtk_container_add(GTK_CONTAINER(generalSet), gSvbox);

        GtkWidget* bitrateCheck = gtk_check_button_new_with_label(_("Enable Dynamic Bitrate Display"));
        widgets.bitrateCheck = bitrateCheck;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bitrateCheck), pluginConfig.dynamicBitrate);
        gtk_box_pack_start(GTK_BOX(gSvbox), bitrateCheck, FALSE, FALSE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), generalSet, gtk_label_new(_("Plugin")));

        //ReplayGain Settings Tab
        GtkWidget* replaygainSet = gtk_frame_new(_("ReplayGain Settings"));
        gtk_container_set_border_width(GTK_CONTAINER(replaygainSet), 5);

        GtkWidget* rSVbox = gtk_vbox_new(FALSE, 10);
        gtk_container_set_border_width(GTK_CONTAINER(rSVbox), 5);
        gtk_container_add(GTK_CONTAINER(replaygainSet), rSVbox);

        GtkWidget* clippingCheck = gtk_check_button_new_with_label(_("Enable Clipping Prevention"));
        widgets.clippingCheck = clippingCheck;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(clippingCheck), pluginConfig.clipPrevention);
        gtk_box_pack_start(GTK_BOX(rSVbox), clippingCheck, FALSE, FALSE, 0);

        GtkWidget* replaygainCheck = gtk_check_button_new_with_label(_("Enable ReplayGain"));
        widgets.replaygainCheck = replaygainCheck;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(replaygainCheck), pluginConfig.replaygain);
        gtk_box_pack_start(GTK_BOX(rSVbox), replaygainCheck, FALSE, FALSE, 0);

        GtkWidget* replaygainType = gtk_frame_new(_("ReplayGain Type"));
        gtk_box_pack_start(GTK_BOX(rSVbox), replaygainType, FALSE, FALSE, 0);
        g_signal_connect(G_OBJECT(replaygainCheck), "toggled", G_CALLBACK(toggleSwitch), replaygainType);

        GtkWidget* rgVbox = gtk_vbox_new(FALSE, 5);
        gtk_container_set_border_width(GTK_CONTAINER(rgVbox), 5);
        gtk_container_add(GTK_CONTAINER(replaygainType), rgVbox);

        GtkWidget* trackCheck = gtk_radio_button_new_with_label(NULL, _("Use Track Gain"));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(trackCheck), !pluginConfig.albumGain);
        gtk_box_pack_start(GTK_BOX(rgVbox), trackCheck, FALSE, FALSE, 0);

        GtkWidget* albumCheck = gtk_radio_button_new_with_label(gtk_radio_button_get_group(GTK_RADIO_BUTTON(trackCheck)), _("Use Album Gain"));
        widgets.albumCheck = albumCheck;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(albumCheck), pluginConfig.albumGain);
        gtk_box_pack_start(GTK_BOX(rgVbox), albumCheck, FALSE, FALSE, 0);
        gtk_widget_set_sensitive(replaygainType, pluginConfig.replaygain);
        gtk_notebook_append_page(GTK_NOTEBOOK(notebook), replaygainSet, gtk_label_new(_("ReplayGain")));

        //Buttons
        GtkWidget* buttonBox = gtk_hbutton_box_new();
        gtk_button_box_set_layout(GTK_BUTTON_BOX(buttonBox), GTK_BUTTONBOX_END);
        gtk_box_set_spacing(GTK_BOX(buttonBox), 5);
        gtk_box_pack_start(GTK_BOX(vbox), buttonBox, FALSE, FALSE, 0);

        GtkWidget* okButton = gtk_button_new_with_label(_("Ok"));
        g_signal_connect(G_OBJECT(okButton), "clicked", G_CALLBACK(saveConfigBox), NULL);
        GTK_WIDGET_SET_FLAGS(okButton, GTK_CAN_DEFAULT);
        gtk_box_pack_start(GTK_BOX(buttonBox), okButton, TRUE, TRUE, 0);

        GtkWidget* cancelButton = gtk_button_new_with_label(_("Cancel"));
        g_signal_connect_swapped(G_OBJECT(cancelButton), "clicked", G_CALLBACK(gtk_widget_destroy), GTK_OBJECT(widgets.configBox));
        GTK_WIDGET_SET_FLAGS(cancelButton, GTK_CAN_DEFAULT);
        gtk_widget_grab_default(cancelButton);
        gtk_box_pack_start(GTK_BOX(buttonBox), cancelButton, TRUE, TRUE, 0);

        gtk_widget_show_all(configBox);
    }
}

static void saveConfigBox(GtkWidget* p_Widget, gpointer p_Data)
{
    mcs_handle_t* cfg;
    GtkToggleButton* tb;

    tb = GTK_TOGGLE_BUTTON(widgets.replaygainCheck);
    pluginConfig.replaygain = gtk_toggle_button_get_active(tb);
    tb = GTK_TOGGLE_BUTTON(widgets.clippingCheck);
    pluginConfig.clipPrevention = gtk_toggle_button_get_active(tb);
    tb = GTK_TOGGLE_BUTTON(widgets.bitrateCheck);
    pluginConfig.dynamicBitrate = gtk_toggle_button_get_active(tb);
    tb = GTK_TOGGLE_BUTTON(widgets.albumCheck);
    pluginConfig.albumGain = gtk_toggle_button_get_active(tb);

    cfg = aud_cfg_db_open();

    aud_cfg_db_set_bool(cfg, "musepack", "clipPrevention", pluginConfig.clipPrevention);
    aud_cfg_db_set_bool(cfg, "musepack", "albumGain",      pluginConfig.albumGain);
    aud_cfg_db_set_bool(cfg, "musepack", "dynamicBitrate", pluginConfig.dynamicBitrate);
    aud_cfg_db_set_bool(cfg, "musepack", "replaygain",     pluginConfig.replaygain);

    aud_cfg_db_close(cfg);

    gtk_widget_destroy (widgets.configBox);
}

static gint mpcIsOurFD(const gchar* p_Filename, VFSFile* file)
{
    gchar magic[4];
    if (4 != vfs_fread(magic, 1, 4, file))
		return 0;
    if (memcmp(magic, "MP+", 3) == 0)
        return 1;
	if (memcmp(magic, "MPCK", 4) == 0)
		return 1;
    return 0;
}

static void mpcPlay(InputPlayback *data)
{
    mpcDecoder.offset   = -1;
    mpcDecoder.isAlive  = true;
    mpcDecoder.isPause  = false;
    threadHandle = g_thread_self();
    decodeStream(data);
}

static void mpcStop(InputPlayback *data)
{
    mpcDecoder.isAlive = false;
}

inline static void lockAcquire()
{
    g_static_mutex_lock(&threadMutex);
}

inline static void lockRelease()
{
    g_static_mutex_unlock(&threadMutex);
}

static void mpcPause(InputPlayback *data, short p_Pause)
{
    lockAcquire();
    mpcDecoder.isPause = p_Pause;
    data->output->pause(p_Pause);
    lockRelease();
}

static void mpcSeekm(InputPlayback *data, gulong ms_offset)
{
    lockAcquire();
    mpcDecoder.offset = ms_offset;
    lockRelease();
}

static void mpcSeek(InputPlayback *data, int p_Offset)
{
	mpcSeekm(data, 1000 * p_Offset);
}

static MpcInfo getTags(const gchar* p_Filename)
{
    gchar *pRealFilename = g_filename_from_uri(p_Filename, NULL, NULL);
    File oFile(pRealFilename ? pRealFilename : p_Filename, false);
    g_free(pRealFilename);
    Tag* poTag = oFile.tag();
    MpcInfo tags = {0};
    tags.title   = g_strdup(poTag->title().toCString(true));
    REMOVE_NONEXISTANT_TAG(tags.title);
    tags.artist  = g_strdup(poTag->artist().toCString(true));
    REMOVE_NONEXISTANT_TAG(tags.artist);
    tags.album   = g_strdup(poTag->album().toCString(true));
    REMOVE_NONEXISTANT_TAG(tags.album);
    tags.genre   = g_strdup(poTag->genre().toCString(true));
    REMOVE_NONEXISTANT_TAG(tags.genre);
    tags.comment = g_strdup(poTag->comment().toCString(true));
    REMOVE_NONEXISTANT_TAG(tags.comment);
    tags.year    = poTag->year();
    tags.track   = poTag->track();
#if 0
    TagLib::APE::Tag* ape = oFile.APETag(false);
    if(ape)
    {
        ItemListMap map = ape->itemListMap();
        if(map.contains("YEAR"))
        {
            tags.date = g_strdup(map["YEAR"].toString().toCString(true));
        }
        else
        {
            tags.date = g_strdup_printf("%d", tags.year);
        }
    }
#endif
    return tags;
}

static void removeTags(GtkWidget * w, gpointer data)
{
    File oFile(gtk_entry_get_text(GTK_ENTRY(widgets.fileEntry)));
    oFile.remove();
    oFile.save();
    closeInfoBox(NULL, NULL);
}

static void saveTags(GtkWidget* w, gpointer data)
{
    File oFile(gtk_entry_get_text(GTK_ENTRY(widgets.fileEntry)));
    Tag* poTag = oFile.tag();

    gchar* cAlbum   = g_strdup(gtk_entry_get_text(GTK_ENTRY(widgets.albumEntry)));
    gchar* cArtist  = g_strdup(gtk_entry_get_text(GTK_ENTRY(widgets.artistEntry)));
    gchar* cTitle   = g_strdup(gtk_entry_get_text(GTK_ENTRY(widgets.titleEntry)));
    gchar* cGenre   = g_strdup(gtk_entry_get_text(GTK_ENTRY(widgets.genreEntry)));
    gchar* cComment = g_strdup(gtk_entry_get_text(GTK_ENTRY(widgets.commentEntry)));

    const String album   = String(cAlbum,   TagLib::String::UTF8);
    const String artist  = String(cArtist,  TagLib::String::UTF8);
    const String title   = String(cTitle,   TagLib::String::UTF8);
    const String genre   = String(cGenre,   TagLib::String::UTF8);
    const String comment = String(cComment, TagLib::String::UTF8);

    poTag->setAlbum(album);
    poTag->setArtist(artist);
    poTag->setTitle(title);
    poTag->setGenre(genre);
    poTag->setComment(comment);
    poTag->setYear(atoi(gtk_entry_get_text(GTK_ENTRY(widgets.yearEntry))));
    poTag->setTrack(atoi(gtk_entry_get_text(GTK_ENTRY(widgets.trackEntry))));

    g_free(cAlbum);
    g_free(cArtist);
    g_free(cTitle);
    g_free(cGenre);
    g_free(cComment);

    oFile.save();
    closeInfoBox(NULL, NULL);
}

static void freeTags(MpcInfo& tags)
{
    g_free(tags.title);
    g_free(tags.artist);
    g_free(tags.album);
    g_free(tags.comment);
    g_free(tags.genre);
    g_free(tags.date);
}

static Tuple *mpcGetTuple(const gchar* p_Filename, VFSFile *input)
{
	Tuple *tuple = 0;
	bool close_input = false;

	if (input == 0) {
		input = vfs_fopen(p_Filename, "rb");
		if (input == 0) {
			gchar* temp = g_strdup_printf("[xmms-musepack] mpcGetTuple is unable to open %s\n", p_Filename);
			perror(temp);
			g_free(temp);
			return 0;
		}
		close_input = true;
	}

	tuple = tuple_new_from_filename(p_Filename);

	mpc_streaminfo info;
	mpc_reader reader;
	mpc_reader_setup_file_vfs(&reader, input);
	mpc_demux * demux = mpc_demux_init(&reader);
	mpc_demux_get_info(demux, &info);
	mpc_demux_exit(demux);

	tuple_associate_int(tuple, FIELD_LENGTH, NULL, static_cast<int> (1000 * mpc_streaminfo_get_length(&info)));

 	gchar *scratch = g_strdup_printf("Musepack v%d (encoder %s)", info.stream_version, info.encoder);
 	tuple_associate_string(tuple, FIELD_CODEC, NULL, scratch);
 	g_free(scratch);

 	scratch = g_strdup_printf("lossy (%s)", info.profile_name);
 	tuple_associate_string(tuple, FIELD_QUALITY, NULL, scratch);
 	g_free(scratch);

	tuple_associate_int(tuple, FIELD_BITRATE, NULL, static_cast<int> (info.average_bitrate / 1000));

	MpcInfo tags = getTags(p_Filename);

 	tuple_associate_string(tuple, FIELD_DATE, NULL, tags.date);
 	tuple_associate_string(tuple, FIELD_TITLE, NULL, tags.title);
 	tuple_associate_string(tuple, FIELD_ARTIST, NULL, tags.artist);
 	tuple_associate_string(tuple, FIELD_ALBUM, NULL, tags.album);
 	tuple_associate_int(tuple, FIELD_TRACK_NUMBER, NULL, tags.track);
 	tuple_associate_int(tuple, FIELD_YEAR, NULL, tags.year);
 	tuple_associate_string(tuple, FIELD_GENRE, NULL, tags.genre);
 	tuple_associate_string(tuple, FIELD_COMMENT, NULL, tags.comment);

	freeTags(tags);

	if (close_input)
		vfs_fclose(input);

	return tuple;
}

static Tuple *mpcProbeForTuple(const gchar* p_Filename, VFSFile *input)
{
    return mpcGetTuple(p_Filename, input);
}

static Tuple *mpcGetSongTuple(const gchar* p_Filename)
{
	return mpcGetTuple(p_Filename, 0);
}

static void mpcGtkPrintLabel(GtkWidget* widget, const char* format,...)
{
    va_list args;

    va_start(args, format);
    gchar* temp = g_strdup_vprintf(format, args);
    va_end(args);

    gtk_label_set_text(GTK_LABEL(widget), temp);
    g_free(temp);
}

static GtkWidget* mpcGtkTagLabel(const char* p_Text, int a, int b, int c, int d, GtkWidget* p_Box)
{
    GtkWidget* label = gtk_label_new(p_Text);
    gtk_misc_set_alignment(GTK_MISC(label), 1, 0.5);
    gtk_table_attach(GTK_TABLE(p_Box), label, a, b, c, d, GTK_FILL, GTK_FILL, 5, 5);
    return label;
}

static GtkWidget* mpcGtkTagEntry(int a, int b, int c, int d, int p_Size, GtkWidget* p_Box)
{
    GtkWidget* entry;
    entry = gtk_entry_new();
    if(p_Size)
        gtk_entry_set_max_length(GTK_ENTRY(entry), p_Size);

    gtk_table_attach(GTK_TABLE(p_Box), entry, a, b, c, d,
                    (GtkAttachOptions) (GTK_FILL | GTK_EXPAND | GTK_SHRINK),
                    (GtkAttachOptions) (GTK_FILL | GTK_EXPAND | GTK_SHRINK), 0, 5);
    return entry;
}

static GtkWidget* mpcGtkLabel(GtkWidget* p_Box)
{
    GtkWidget* label = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(label), 0, 0);
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_LEFT);
    gtk_box_pack_start(GTK_BOX(p_Box), label, FALSE, FALSE, 0);
    return label;
}

static GtkWidget* mpcGtkButton(const char* p_Text, GtkWidget* p_Box)
{
    GtkWidget* button = gtk_button_new_with_label(p_Text);
    GTK_WIDGET_SET_FLAGS(button, GTK_CAN_DEFAULT);
    gtk_box_pack_start(GTK_BOX(p_Box), button, TRUE, TRUE, 0);
    return button;
}

static void mpcFileInfoBox(const gchar* p_Filename)
{
    GtkWidget* infoBox = widgets.infoBox;

    if(infoBox)
        gdk_window_raise(infoBox->window);
    else
    {
        infoBox = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_type_hint(GTK_WINDOW(infoBox), GDK_WINDOW_TYPE_HINT_DIALOG);
        widgets.infoBox = infoBox;
        gtk_window_set_resizable(GTK_WINDOW(infoBox), FALSE);
        g_signal_connect(G_OBJECT(infoBox), "destroy", G_CALLBACK(closeInfoBox), NULL);
        gtk_container_set_border_width(GTK_CONTAINER(infoBox), 10);

        GtkWidget* iVbox = gtk_vbox_new(FALSE, 10);
        gtk_container_add(GTK_CONTAINER(infoBox), iVbox);

        GtkWidget* filenameHbox = gtk_hbox_new(FALSE, 5);
        gtk_box_pack_start(GTK_BOX(iVbox), filenameHbox, FALSE, TRUE, 0);

        GtkWidget* fileLabel = gtk_label_new(_("Filename:"));
        gtk_box_pack_start(GTK_BOX(filenameHbox), fileLabel, FALSE, TRUE, 0);

        GtkWidget* fileEntry = gtk_entry_new();
        widgets.fileEntry = fileEntry;
        gtk_editable_set_editable(GTK_EDITABLE(fileEntry), FALSE);
        gtk_box_pack_start(GTK_BOX(filenameHbox), fileEntry, TRUE, TRUE, 0);

        GtkWidget* iHbox = gtk_hbox_new(FALSE, 10);
        gtk_box_pack_start(GTK_BOX(iVbox), iHbox, FALSE, TRUE, 0);

        GtkWidget* leftBox = gtk_vbox_new(FALSE, 10);
        gtk_box_pack_start(GTK_BOX(iHbox), leftBox, FALSE, FALSE, 0);

        //Tag labels
        GtkWidget* tagFrame = gtk_frame_new(_("Musepack Tag"));
        gtk_box_pack_start(GTK_BOX(leftBox), tagFrame, FALSE, FALSE, 0);
        gtk_widget_set_sensitive(tagFrame, TRUE);

        GtkWidget* iTable = gtk_table_new(5, 5, FALSE);
        gtk_container_set_border_width(GTK_CONTAINER(iTable), 5);
        gtk_container_add(GTK_CONTAINER(tagFrame), iTable);

        mpcGtkTagLabel(_("Title:"), 0, 1, 0, 1, iTable);
        GtkWidget* titleEntry = mpcGtkTagEntry(1, 4, 0, 1, 0, iTable);
        widgets.titleEntry = titleEntry;

        mpcGtkTagLabel(_("Artist:"), 0, 1, 1, 2, iTable);
        GtkWidget* artistEntry = mpcGtkTagEntry(1, 4, 1, 2, 0, iTable);
        widgets.artistEntry = artistEntry;

        mpcGtkTagLabel(_("Album:"), 0, 1, 2, 3, iTable);
        GtkWidget* albumEntry = mpcGtkTagEntry(1, 4, 2, 3, 0, iTable);
        widgets.albumEntry = albumEntry;

        mpcGtkTagLabel(_("Comment:"), 0, 1, 3, 4, iTable);
        GtkWidget* commentEntry = mpcGtkTagEntry(1, 4, 3, 4, 0, iTable);
        widgets.commentEntry = commentEntry;

        mpcGtkTagLabel(_("Year:"), 0, 1, 4, 5, iTable);
        GtkWidget* yearEntry = mpcGtkTagEntry(1, 2, 4, 5, 4, iTable);
        widgets.yearEntry = yearEntry;
        gtk_widget_set_size_request(yearEntry, 4, -1);

        mpcGtkTagLabel(_("Track:"), 2, 3, 4, 5, iTable);
        GtkWidget* trackEntry = mpcGtkTagEntry(3, 4, 4, 5, 4, iTable);
        widgets.trackEntry = trackEntry;
        gtk_widget_set_size_request(trackEntry, 3, -1);

        mpcGtkTagLabel(_("Genre:"), 0, 1, 5, 6, iTable);
        GtkWidget* genreEntry = mpcGtkTagEntry(1, 4, 5, 6, 0, iTable);
        widgets.genreEntry = genreEntry;
        gtk_widget_set_size_request(genreEntry, 20, -1);

        //Buttons
        GtkWidget* buttonBox = gtk_hbutton_box_new();
        gtk_button_box_set_layout(GTK_BUTTON_BOX(buttonBox), GTK_BUTTONBOX_END);
        gtk_box_set_spacing(GTK_BOX(buttonBox), 5);
        gtk_box_pack_start(GTK_BOX(leftBox), buttonBox, FALSE, FALSE, 0);

        GtkWidget* saveButton = mpcGtkButton(_("Save"), buttonBox);
        g_signal_connect(G_OBJECT(saveButton), "clicked", G_CALLBACK(saveTags), NULL);

        GtkWidget* removeButton = mpcGtkButton(_("Remove Tag"), buttonBox);
        g_signal_connect_swapped(G_OBJECT(removeButton), "clicked", G_CALLBACK(removeTags), NULL);

        GtkWidget* cancelButton = mpcGtkButton(_("Cancel"), buttonBox);
        g_signal_connect_swapped(G_OBJECT(cancelButton), "clicked", G_CALLBACK(closeInfoBox), NULL);
        gtk_widget_grab_default(cancelButton);

        //File information
        GtkWidget* infoFrame = gtk_frame_new(_("Musepack Info"));
        gtk_box_pack_start(GTK_BOX(iHbox), infoFrame, FALSE, FALSE, 0);

        GtkWidget* infoVbox = gtk_vbox_new(FALSE, 5);
        gtk_container_add(GTK_CONTAINER(infoFrame), infoVbox);
        gtk_container_set_border_width(GTK_CONTAINER(infoVbox), 10);
        gtk_box_set_spacing(GTK_BOX(infoVbox), 0);

        GtkWidget* streamLabel    = mpcGtkLabel(infoVbox);
        GtkWidget* encoderLabel   = mpcGtkLabel(infoVbox);
        GtkWidget* profileLabel   = mpcGtkLabel(infoVbox);
		GtkWidget* pnsLabel       = mpcGtkLabel(infoVbox);
		GtkWidget* gaplessLabel   = mpcGtkLabel(infoVbox);
        GtkWidget* bitrateLabel   = mpcGtkLabel(infoVbox);
        GtkWidget* rateLabel      = mpcGtkLabel(infoVbox);
        GtkWidget* channelsLabel  = mpcGtkLabel(infoVbox);
        GtkWidget* lengthLabel    = mpcGtkLabel(infoVbox);
        GtkWidget* fileSizeLabel  = mpcGtkLabel(infoVbox);
        GtkWidget* trackPeakLabel = mpcGtkLabel(infoVbox);
        GtkWidget* trackGainLabel = mpcGtkLabel(infoVbox);
        GtkWidget* albumPeakLabel = mpcGtkLabel(infoVbox);
        GtkWidget* albumGainLabel = mpcGtkLabel(infoVbox);

        VFSFile *input = vfs_fopen(p_Filename, "rb");
        if(input)
        {
            mpc_streaminfo info;
            mpc_reader reader;
            mpc_reader_setup_file_vfs(&reader, input);
			mpc_demux * demux = mpc_demux_init(&reader);
			mpc_demux_get_info(demux, &info);
			mpc_demux_exit(demux);

            gint time = static_cast<int> (mpc_streaminfo_get_length(&info));
            gint minutes = time / 60;
            gint seconds = time % 60;

			mpcGtkPrintLabel(streamLabel,    _("Streamversion %d"), info.stream_version);
			mpcGtkPrintLabel(encoderLabel,   _("Encoder: \%s"), info.encoder);
			mpcGtkPrintLabel(profileLabel,   _("Profile: \%s (q=%0.2f)"), info.profile_name, info.profile - 5);
			mpcGtkPrintLabel(pnsLabel,       _("PNS: \%s"), info.pns == 0xFF ? _("unknow") : info.pns ? _("on") : _("off"));
			mpcGtkPrintLabel(gaplessLabel,   _("Gapless: \%s"), info.is_true_gapless ? _("on") : _("off"));
			mpcGtkPrintLabel(bitrateLabel,   _("Average bitrate: \%6.1f kbps"), info.average_bitrate * 1.e-3);
			mpcGtkPrintLabel(rateLabel,      _("Samplerate: \%d Hz"), info.sample_freq);
			mpcGtkPrintLabel(channelsLabel,  _("Channels: \%d"), info.channels);
			mpcGtkPrintLabel(lengthLabel,    _("Length: \%d:\%.2d (%u samples)"), minutes, seconds, (mpc_uint32_t)mpc_streaminfo_get_length_samples(&info));
			mpcGtkPrintLabel(fileSizeLabel,  _("File size: \%d Bytes"), info.total_file_length);
			mpcGtkPrintLabel(trackPeakLabel, _("Track Peak: \%2.2f dB"), info.peak_title / 256.);
			mpcGtkPrintLabel(trackGainLabel, _("Track Gain: \%2.2f dB"), info.gain_title / 256.);
			mpcGtkPrintLabel(albumPeakLabel, _("Album Peak: \%2.2f dB"), info.peak_album / 256.);
			mpcGtkPrintLabel(albumGainLabel, _("Album Gain: \%2.2f dB"), info.gain_album / 256.);

            MpcInfo tags = getTags(p_Filename);
            gtk_entry_set_text(GTK_ENTRY(titleEntry),   tags.title);
            gtk_entry_set_text(GTK_ENTRY(artistEntry),  tags.artist);
            gtk_entry_set_text(GTK_ENTRY(albumEntry),   tags.album);
            gtk_entry_set_text(GTK_ENTRY(commentEntry), tags.comment);
            gtk_entry_set_text(GTK_ENTRY(genreEntry),   tags.genre);
            gchar* entry = g_strdup_printf ("%d", tags.track);
            gtk_entry_set_text(GTK_ENTRY(trackEntry), entry);
            g_free(entry);
            entry = g_strdup_printf ("%d", tags.year);
            gtk_entry_set_text(GTK_ENTRY(yearEntry), entry);
            g_free(entry);
            entry = g_filename_display_name(p_Filename);
            gtk_entry_set_text(GTK_ENTRY(fileEntry), entry);
            g_free(entry);
            freeTags(tags);
            vfs_fclose(input);
        }
        else
        {
            gchar* temp = g_strdup_printf("[xmms-musepack] mpcFileInfoBox is unable to read tags from %s", p_Filename);
            perror(temp);
            g_free(temp);
        }

	gchar* name = g_filename_display_basename(p_Filename);
        gchar* text = g_strdup_printf(_("File Info - %s"), name);
        g_free(name);
        gtk_window_set_title(GTK_WINDOW(infoBox), text);
        g_free(text);

        gtk_widget_show_all(infoBox);
    }
}

static void closeInfoBox(GtkWidget* w, gpointer data)
{
    gtk_widget_destroy(widgets.infoBox);
    widgets.infoBox = NULL;
}

static void* endThread(gchar* p_FileName, VFSFile * p_FileHandle, bool release)
{
    if (release)
        lockRelease();
    if (mpcDecoder.isError)
    {
        perror(mpcDecoder.isError);
        g_free(mpcDecoder.isError);
        mpcDecoder.isError = NULL;
    }
    mpcDecoder.isAlive = false;
    if(p_FileHandle)
        vfs_fclose(p_FileHandle);
    return 0;
}

static int processBuffer(InputPlayback *playback,
    MPC_SAMPLE_FORMAT* sampleBuffer, char* xmmsBuffer, mpc_demux* demux)
{
	mpc_frame_info info;

	info.buffer = sampleBuffer;
	mpc_demux_decode(demux, &info);

	if (info.bits == -1) return -1; // end of stream

	copyBuffer(sampleBuffer, xmmsBuffer, info.samples * streamInfo.channels);

    if (pluginConfig.dynamicBitrate)
    {
		mpcDecoder.dynbitrate = info.bits * streamInfo.sample_freq / 1152;
    }

	playback->output->write_audio(xmmsBuffer, info.samples * 2 * streamInfo.channels);
	return info.samples;
}

static void* decodeStream(InputPlayback *data)
{
    lockAcquire();
    gchar* filename = data->filename;
    VFSFile *input = vfs_fopen(filename, "rb");
    if (!input)
    {
        mpcDecoder.isError = g_strdup_printf("[xmms-musepack] decodeStream is unable to open %s", filename);
        return endThread(filename, input, true);
    }

    mpc_reader reader;
    mpc_reader_setup_file_vfs(&reader, input);

	mpc_demux * demux = mpc_demux_init(&reader);
	if (demux == 0)
	{
		mpcDecoder.isError = g_strdup_printf("[xmms-musepack] decodeStream is unable to initialize demuxer on %s", filename);
		return endThread(filename, input, true);
	}

	mpc_demux_get_info(demux, &streamInfo);

	data->set_tuple(data, mpcGetTuple(data->filename, input));
	data->set_params(data, 0, 0, static_cast<int> (streamInfo.average_bitrate),
					 streamInfo.sample_freq, streamInfo.channels);

	mpc_set_replay_level(demux, MPC_OLD_GAIN_REF, pluginConfig.replaygain,
						 pluginConfig.albumGain, pluginConfig.clipPrevention);

    MPC_SAMPLE_FORMAT sampleBuffer[MPC_DECODER_BUFFER_LENGTH];
    char xmmsBuffer[MPC_DECODER_BUFFER_LENGTH * 4];

    if (!data->output->open_audio(FMT_S16_LE, streamInfo.sample_freq, streamInfo.channels))
    {
        mpcDecoder.isError = g_strdup_printf("[xmms-musepack] decodeStream is unable to open an audio output");
        return endThread(filename, input, true);
    }

    lockRelease();

	data->set_pb_ready(data);
	data->playing = TRUE;

    gint counter = 2 * streamInfo.sample_freq / 3;
	int status = 0;
    while (mpcDecoder.isAlive)
    {
        lockAcquire();

		if (mpcDecoder.offset != -1)
        {
			mpc_demux_seek_sample(demux, mpcDecoder.offset * streamInfo.sample_freq / 1000);
			data->output->flush(mpcDecoder.offset);
            mpcDecoder.offset = -1;
        }
		lockRelease();

        if (mpcDecoder.isPause == FALSE && status != -1)
        {
            status = processBuffer(data, sampleBuffer, xmmsBuffer, demux);

            if(pluginConfig.dynamicBitrate)
            {
                counter -= status;
                if(counter < 0)
                {
					data->set_params(data, 0, 0, mpcDecoder.dynbitrate, streamInfo.sample_freq, streamInfo.channels);
                    counter = 2 * streamInfo.sample_freq / 3;
                }
            }
        }
        else
        {
			if (mpcDecoder.isPause == FALSE && status == -1 &&
						 data->output->buffer_playing() == FALSE)
				break;
            g_usleep(60000);
        }
    }
	data->output->close_audio();
	mpc_demux_exit(demux);
    return endThread(filename, input, false);
}

const gchar *mpc_fmts[] = { "mpc", NULL };

InputPlugin MpcPlugin = {
	0, // handle
	0, // filename
    (gchar *)"Musepack Audio Plugin", // description
    mpcOpenPlugin, // init
	0, // cleanup
    mpcAboutBox, // about : Show About box
    mpcConfigBox, // configure : Show Configure box
    0, // PluginPreferences *settings
	0, // sendmsg

    0, // gboolean have_subtune : Plugin supports/uses subtunes.
    (gchar **)mpc_fmts, // vfs_extensions
    0, // priority
    mpcIsOurFD, // is_our_file_from_vfs
    mpcGetSongTuple, // get_song_tuple : Acquire tuple for song
	mpcProbeForTuple, // Tuple *(*probe_for_tuple)(gchar *uri, VFSFile *fd);
    0, //     gboolean (*update_song_tuple)(Tuple *tuple, VFSFile *fd);
	mpcFileInfoBox, // file_info_box : Show File Info Box
	0, // get_song_image
	0, // play
    mpcPause, // pause
	mpcSeekm, // void (*mseek) (InputPlayback * playback, gulong millisecond);
    mpcStop, // stop
	0, // get_time
	0, // get_volume
	0, // set_volume
	
	/* Deprecated */
	0, // is_our_file
    mpcPlay, // play_file
    mpcSeek, // seek
};

InputPlugin *mpc_iplist[] = { &MpcPlugin, NULL };

DECLARE_PLUGIN(musepack, NULL, NULL, mpc_iplist, NULL, NULL, NULL, NULL,NULL);
