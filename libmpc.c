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

#define VERSION "1.3"
#define PACKAGE "aud_mpc"

static MpcDecoder		mpcDecoder = {0};
static mpc_streaminfo	streamInfo = {0};
static GThread*			threadHandle;
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

static gboolean mpcOpenPlugin()
{
	return TRUE;
}

static void mpcAboutBox()
{
 	static GtkWidget* aboutBox = NULL;
    char* titleText = g_strdup_printf(_("Musepack Decoder Plugin %s"), VERSION);
    const char* contentText = _("Plugin code by\nBenoit Amiaux\nMartin Spuler\nKuniklo\nNicolas Botti\n\nGet latest version at http://musepack.net\n");
	audgui_simple_message (&aboutBox, GTK_MESSAGE_INFO, titleText, contentText);
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

static gboolean mpcPlay(InputPlayback * playback, const char * filename,
VFSFile * file, int start_time, int stop_time, bool_t pause)
{
    mpcDecoder.offset   = -1;
    mpcDecoder.isAlive  = TRUE;
    mpcDecoder.isPause  = FALSE;
    threadHandle = g_thread_self();
    return decodeStream(playback, filename);
}

static void mpcStop(InputPlayback *data)
{
    mpcDecoder.isAlive = FALSE;
}

inline static void lockAcquire()
{
    g_static_mutex_lock(&threadMutex);
}

inline static void lockRelease()
{
    g_static_mutex_unlock(&threadMutex);
}

static void mpcPause(InputPlayback *data, gboolean p_Pause)
{
    lockAcquire();
    mpcDecoder.isPause = p_Pause;
    data->output->pause(p_Pause);
    lockRelease();
}

static void mpcSeekm(InputPlayback *data, int ms_offset)
{
    lockAcquire();
    mpcDecoder.offset = ms_offset;
    lockRelease();
}

static bool_t mpcUpdateSongTuple(const Tuple * tuple, VFSFile * file)
{
	return tag_tuple_write(tuple, file, TAG_TYPE_APE);
}

static Tuple *mpcGetTuple(const gchar* p_Filename, VFSFile *input)
{
	Tuple *tuple = 0;
	gboolean close_input = FALSE;

	if (input == 0) {
		input = vfs_fopen(p_Filename, "rb");
		if (input == 0) {
			gchar* temp = g_strdup_printf("[aud_mpc] mpcGetTuple is unable to open %s\n", p_Filename);
			perror(temp);
			g_free(temp);
			return 0;
		}
		close_input = TRUE;
	}

	tuple = tuple_new_from_filename(p_Filename);

	mpc_streaminfo info;
	mpc_reader reader;
	mpc_reader_setup_file_vfs(&reader, input);
	mpc_demux * demux = mpc_demux_init(&reader);
	mpc_demux_get_info(demux, &info);
	mpc_demux_exit(demux);

	tuple_set_int(tuple, FIELD_LENGTH, NULL, (int)(1000 * mpc_streaminfo_get_length(&info)));

 	gchar *scratch = g_strdup_printf("Musepack SV%d (encoder %s)", info.stream_version, info.encoder);
 	tuple_set_str(tuple, FIELD_CODEC, NULL, scratch);
 	g_free(scratch);

 	scratch = g_strdup_printf("lossy (%s)", info.profile_name);
 	tuple_set_str(tuple, FIELD_QUALITY, NULL, scratch);
 	g_free(scratch);

	tuple_set_int(tuple, FIELD_BITRATE, NULL, (int)(info.average_bitrate / 1000));

	if (! vfs_is_streaming (input))	{
		vfs_rewind (input);
		tag_tuple_read (tuple, input);
	}
	
	tuple_set_str(tuple, FIELD_MIMETYPE, NULL, "audio/x-musepack");

	if (close_input) {
		vfs_fclose(input);
	}
	return tuple;
}

static gboolean mpcUpdateReplayGain(mpc_streaminfo* streamInfo, ReplayGainInfo* rg_info)
{
    if ((streamInfo == NULL) || (rg_info == NULL)) {
        return FALSE;
    }

    rg_info->track_gain = streamInfo->gain_title / 100.0;
    rg_info->album_gain = streamInfo->gain_album / 100.0;
    rg_info->track_peak = streamInfo->peak_title / 65535.0;
    rg_info->album_peak = streamInfo->peak_album / 65535.0;

    return TRUE;
}

static void endThread(VFSFile * p_FileHandle, gboolean release)
{
    if (release)
        lockRelease();
    if (mpcDecoder.isError)
    {
        perror(mpcDecoder.isError);
        g_free(mpcDecoder.isError);
        mpcDecoder.isError = NULL;
    }
    mpcDecoder.isAlive = FALSE;
    if(p_FileHandle)
        vfs_fclose(p_FileHandle);
}

static int processBuffer(InputPlayback* playback,
MPC_SAMPLE_FORMAT* sampleBuffer, char* xmmsBuffer, mpc_demux* demux)
{
	mpc_frame_info info;

	info.buffer = sampleBuffer;
	mpc_demux_decode(demux, &info);

	if (info.bits == -1) {
		mpcDecoder.isError = g_strdup_printf("[aud_mpc] demux_decode failed  sampleBuffer@%p xmmsBuffer@%p demux@%p", sampleBuffer, xmmsBuffer, demux);
		return -1; // end of stream
	}

	copyBuffer(sampleBuffer, xmmsBuffer, info.samples * streamInfo.channels);

#ifdef AUD_MPC_DYNBITRATE
	mpcDecoder.dynbitrate = info.bits * streamInfo.sample_freq / 1152;
#endif

	playback->output->write_audio(xmmsBuffer, info.samples * 2 * streamInfo.channels);
	return info.samples;
}

static gboolean decodeStream(InputPlayback *data, const char * filename)
{
    lockAcquire();
    VFSFile *input = vfs_fopen(filename, "rb");
    if (!input)
    {
        mpcDecoder.isError = g_strdup_printf("[aud_mpc] decodeStream is unable to open %s", filename);
        endThread(input, TRUE);
        return FALSE;
    }

    mpc_reader reader;
    mpc_reader_setup_file_vfs(&reader, input);

	mpc_demux * demux = mpc_demux_init(&reader);
	if (demux == 0)
	{
		mpcDecoder.isError = g_strdup_printf("[aud_mpc] decodeStream is unable to initialize demuxer on %s", filename);
		endThread(input, TRUE);
		return FALSE;
	}

	mpc_demux_get_info(demux, &streamInfo);

	data->set_tuple(data, mpcGetTuple(filename, input));
	data->set_params(data, (int) (streamInfo.average_bitrate),
					 streamInfo.sample_freq, streamInfo.channels);
					 
    ReplayGainInfo rg_info;
    mpcUpdateReplayGain(&streamInfo, &rg_info);
    data->output->set_replaygain_info(&rg_info);

    MPC_SAMPLE_FORMAT sampleBuffer[MPC_DECODER_BUFFER_LENGTH];
    char xmmsBuffer[MPC_DECODER_BUFFER_LENGTH * 4];

    if (!data->output->open_audio(FMT_S16_LE, streamInfo.sample_freq, streamInfo.channels))
    {
        mpcDecoder.isError = g_strdup_printf("[aud_mpc] decodeStream is unable to open an audio output");
        endThread(input, TRUE);
        return FALSE;
    }

    mpc_demux_seek_sample(demux, 0);

    lockRelease();

	data->set_pb_ready(data);

#ifdef AUD_MPC_DYNBITRATE
    gint counter = 2 * streamInfo.sample_freq / 3;
#endif
	int status = 0;
    while (mpcDecoder.isAlive)
    {
        lockAcquire();
		if (mpcDecoder.offset != -1)
        {
        	g_strdup_printf("[aud_mpc] decodeStream resetting to sample %lld (offset %lld)", mpcDecoder.offset * streamInfo.sample_freq / 1000, mpcDecoder.offset);
			mpc_demux_seek_sample(demux, mpcDecoder.offset * streamInfo.sample_freq / 1000);
			data->output->flush(mpcDecoder.offset);
            mpcDecoder.offset = -1;
        }
		lockRelease();

        if (mpcDecoder.isPause == FALSE && status != -1)
        {
            status = processBuffer(data, sampleBuffer, xmmsBuffer, demux);
#ifdef AUD_MPC_DYNBITRATE
            counter -= status;
            if(counter < 0)
            {
				data->set_params(data, mpcDecoder.dynbitrate, streamInfo.sample_freq, streamInfo.channels);
                counter = 2 * streamInfo.sample_freq / 3;
            }
#endif
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
    endThread(input, FALSE);
    return TRUE;
}

static const gchar *mpc_fmts[] = { "mpc", NULL };
static const gchar * const mimes[] = {"audio/x-musepack", NULL};

AUD_INPUT_PLUGIN
(
    .name = "Musepack",
    .init = mpcOpenPlugin,
    .about = mpcAboutBox,
    .play = mpcPlay,
    .stop = mpcStop,
    .pause = mpcPause,
    .mseek = mpcSeekm,
    .probe_for_tuple = mpcGetTuple,
    .update_song_tuple = mpcUpdateSongTuple,
    .is_our_file_from_vfs = mpcIsOurFD,
    .extensions = mpc_fmts,
    .mimes = mimes,
)
