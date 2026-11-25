/*******************************************************************************#
#           guvcview              http://guvcview.sourceforge.net               #
#                                                                               #
#           Paulo Assis <pj.assis@gmail.com>                                    #
#           Nobuhiro Iwamatsu <iwamatsu@nigauri.org>                            #
#                             Add UYVY color support(Macbook iSight)            #
#                                                                               #
# This program is free software; you can redistribute it and/or modify          #
# it under the terms of the GNU General Public License as published by          #
# the Free Software Foundation; either version 2 of the License, or             #
# (at your option) any later version.                                           #
#                                                                               #
# This program is distributed in the hope that it will be useful,               #
# but WITHOUT ANY WARRANTY; without even the implied warranty of                #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                 #
# GNU General Public License for more details.                                  #
#                                                                               #
# You should have received a copy of the GNU General Public License             #
# along with this program; if not, write to the Free Software                   #
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA     #
#                                                                               #
********************************************************************************/

/* support for internationalization - i18n */
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include "gview.h"
#include "frame_decoder.h"
#include "cameraconfig.h"

#include "uvc_h264.h"
#include "v4l2_formats.h"

#include <ucodec/ucodec.h>
// GUID of the UVC H.264 extension unit: {A29E7641-DE04-47E3-8B2B-F4341AFF003B}
#define GUID_UVCX_H264_XU {0x41, 0x76, 0x9E, 0xA2, 0x04, 0xDE, 0xE3, 0x47, 0x8B, 0x2B, 0xF4, 0x34, 0x1A, 0xFF, 0x00, 0x3B}

extern int verbosity;

typedef struct _h264_decoder_context_t
{
	ucodec_decoder *decoder;

	int width;
	int height;
	int pic_size;      /* 紧凑 YUV420P 大小 */

	uint8_t *out_buf;  /* 当前解码输出目标 */
	int out_size;
	int got_frame;

} h264_decoder_context_t;

static h264_decoder_context_t *h264_ctx = NULL;

/*h264 support type*/
static int h264_support = H264_NONE; /*none by default*/

/*
 * request a IDR frame from the H264 encoder
 * args:
 *   vd - pointer to video device data
 *
 * asserts:
 *   vd is not null
 *
 * returns: none
 */
void h264_request_idr(v4l2_dev_t *vd)
{
	request_h264_frame_type(vd, PICTURE_TYPE_IDR_FULL);
}

/*
 * get h264 support type
 * args:
 *    none
 *
 * asserts:
 *    none
 *
 * returns: support type (H264_NONE; H264_MUXED; H264_FRAME)
 */
int h264_get_support()
{
	return h264_support;
}

/*
 * print probe/commit data
 * args:
 *   data - pointer to probe/commit config data
 *
 * asserts:
 *   data is not null
 *
 * returns: void
 */
static void print_probe_commit_data(uvcx_video_config_probe_commit_t *data)
{
	/*asserts*/
	assert(data != NULL);

	printf("uvcx_video_config_probe_commit:\n");
    printf("\tFrameInterval: %u\n", data->dwFrameInterval);
    printf("\tBitRate: %u\n", data->dwBitRate);
	printf("\tHints: 0x%X\n", data->bmHints);
	printf("\tConfigurationIndex: %i\n", data->wConfigurationIndex);
	printf("\tWidth: %i\n", data->wWidth);
	printf("\tHeight: %i\n", data->wHeight);
	printf("\tSliceUnits: %i\n", data->wSliceUnits);
	printf("\tSliceMode: %i\n", data->wSliceMode);
	printf("\tProfile: %i\n", data->wProfile);
	printf("\tIFramePeriod: %i\n", data->wIFramePeriod);
	printf("\tEstimatedVideoDelay: %i\n",data->wEstimatedVideoDelay);
	printf("\tEstimatedMaxConfigDelay: %i\n",data->wEstimatedMaxConfigDelay);
	printf("\tUsageType: %i\n",data->bUsageType);
	printf("\tRateControlMode: %i\n",data->bRateControlMode);
	printf("\tTemporalScaleMode: %i\n",data->bTemporalScaleMode);
	printf("\tSpatialScaleMode: %i\n",data->bSpatialScaleMode);
	printf("\tSNRScaleMode: %i\n",data->bSNRScaleMode);
	printf("\tStreamMuxOption: %i\n",data->bStreamMuxOption);
	printf("\tStreamFormat: %i\n",data->bStreamFormat);
	printf("\tEntropyCABAC: %i\n",data->bEntropyCABAC);
	printf("\tTimestamp: %i\n",data->bTimestamp);
	printf("\tNumOfReorderFrames: %i\n",data->bNumOfReorderFrames);
	printf("\tPreviewFlipped: %i\n",data->bPreviewFlipped);
	printf("\tView: %i\n",data->bView);
	printf("\tReserved1: %i\n",data->bReserved1);
	printf("\tReserved2: %i\n",data->bReserved2);
	printf("\tStreamID: %i\n",data->bStreamID);
	printf("\tSpatialLayerRatio: %i\n",data->bSpatialLayerRatio);
	printf("\tLeakyBucketSize: %i\n",data->wLeakyBucketSize);
}

/*
 * resets the h264 encoder
 * args:
 *   vd - pointer to video device data
 *
 * asserts:
 *   vd is not null
 *
 * returns: 0 on success or error code on fail
 */
static int uvcx_video_encoder_reset(v4l2_dev_t *vd)
{
	/*assertions*/
	assert(vd != NULL);

	uvcx_encoder_reset encoder_reset_req = {0};

	int err = 0;

	if((err = v4l2core_query_xu_control(
		vd,
		vd->h264_unit_id,
		UVCX_ENCODER_RESET,
		UVC_SET_CUR,
		&encoder_reset_req)) < 0)
		fprintf(stderr, "V4L2_CORE: (UVCX_ENCODER_RESET) error: %s\n", strerror(errno));

	return err;
}

/*
 * probes the h264 encoder config
 * args:
 *   vd - pointer to video device data
 *   query - probe query
 *   uvcx_video_config - pointer to probe/commit config data
 *
 * asserts:
 *   vd is not null
 *
 * returns: 0 on success or error code on fail
 */
static int uvcx_video_probe(v4l2_dev_t *vd, uint8_t query, uvcx_video_config_probe_commit_t *uvcx_video_config)
{
	/*assertions*/
	assert(vd != NULL);

	int err = 0;


	if((err = v4l2core_query_xu_control(
		vd,
		vd->h264_unit_id,
		UVCX_VIDEO_CONFIG_PROBE,
		query,
		uvcx_video_config)) < 0)
		fprintf(stderr, "V4L2_CORE: (UVCX_VIDEO_CONFIG_PROBE) error: %s\n", strerror(errno));

	return err;
}

/*
 * commits the h264 encoder config
 * args:
 *   vd - pointer to video device data
 *   uvcx_video_config - pointer to probe/commit config data
 *
 * asserts:
 *   vd is not null
 *
 * returns: 0 on success or error code on fail
 */
static int uvcx_video_commit(v4l2_dev_t *vd, uvcx_video_config_probe_commit_t *uvcx_video_config)
{
	/*assertions*/
	assert(vd != NULL);

	int err = 0;

	if((err = v4l2core_query_xu_control(
		vd,
		vd->h264_unit_id,
		UVCX_VIDEO_CONFIG_COMMIT,
		UVC_SET_CUR,
		uvcx_video_config)) < 0)
		fprintf(stderr, "V4L2_CORE: (UVCX_VIDEO_CONFIG_COMMIT) error: %s\n", strerror(errno));

	return err;
}


/*
 * gets the uvc h264 xu control unit id, if any
 * args:
 *   vd - pointer to video device data
 *
 * asserts:
 *   vd is not null
 *   device_list->list_devices is not null
 *
 * returns: unit id or 0 if none
 *  (also sets vd->h264_unit_id)
 */
uint8_t get_uvc_h624_unit_id (v4l2_dev_t *vd)
{
	if(verbosity > 1)
		printf("V4L2_CORE: checking for UVCX_H264 unit id\n");

	uint8_t guid[16] = GUID_UVCX_H264_XU;
	vd->h264_unit_id = get_guid_unit_id (vd, guid);

	return vd->h264_unit_id;
}

/*
 * check for uvc h264 support by querying UVCX_VERSION
 * although geting a unit id > 0 from xu_get_unit_id
 * should be enought
 * args:
 *   vd - pointer to video device data
 *
 * asserts:
 *   vd is not null
 *   vd->fd is valid ( > 0 )
 *
 * returns: 1 if support available or 0 otherwise
 */
int check_h264_support(v4l2_dev_t *vd)
{
	/*assertions*/
	assert(vd != NULL);
	assert(vd->fd > 0);

	if(vd->h264_unit_id <= 0)
	{
		if(verbosity > 0)
			printf("V4L2_CORE: device doesn't seem to support uvc H264 (%i)\n", vd->h264_unit_id);
		return 0;
	}

	uvcx_version_t uvcx_version;

	if(v4l2core_query_xu_control(
		vd,
		vd->h264_unit_id,
		UVCX_VERSION,
		UVC_GET_CUR,
		&uvcx_version) < 0)
	{
		if(verbosity > 0)
			printf("V4L2_CORE: device doesn't seem to support uvc H264 in unit_id %d\n", vd->h264_unit_id);
		return 0;
	}

	if(verbosity > 0)
		printf("V4L2_CORE: device seems to support uvc H264 (version: %d) in unit_id %d\n", uvcx_version.wVersion, vd->h264_unit_id);
	return 1;

}

/*
 * adds h264 to the format list, if supported by device
 * args:
 *   vd - pointer to video device data
 *
 * asserts:
 *   vd is not null
 *   vd->list_stream_formats is not null
 *
 * returns: void
 */
void add_h264_format(v4l2_dev_t *vd)
{
	/*assertions*/
	assert(vd != NULL);
	assert(vd->list_stream_formats != NULL);

	if(verbosity > 0)
		printf("V4L2_CORE: checking muxed H264 format support\n");

	if(v4l2core_get_frame_format_index(vd, V4L2_PIX_FMT_H264) >= 0)
	{
		if(verbosity > 0)
			printf("V4L2_CORE: H264 format already in list\n");

		h264_support = H264_FRAME;
		/*check the h264 unit id (if any) */
		get_uvc_h624_unit_id(vd);

		return; /*H264 is already in the list*/
	}

	if(get_uvc_h624_unit_id(vd) <= 0)
	{
		h264_support = H264_NONE;
		return; /*no unit id found for h264*/
	}

	if(!check_h264_support(vd))
	{
		h264_support = H264_NONE;
		return; /*no XU support for h264*/
	}

	int mjpg_index = v4l2core_get_frame_format_index(vd, V4L2_PIX_FMT_MJPEG);
	if(mjpg_index < 0) /*MJPG must be available for muxed uvc H264 streams*/
		return;

	/*add the format to the list*/
	if(verbosity > 0)
		printf("V4L2_CORE: adding muxed H264 format\n");

	/*if we got here then muxed h264 is supported*/
	h264_support = H264_MUXED;

	vd->numb_formats++; /*increment number of formats*/
	int fmtind = vd->numb_formats;

	vd->list_stream_formats = realloc(
		vd->list_stream_formats,
        (size_t)fmtind * sizeof(v4l2_stream_formats_t));
	if(vd->list_stream_formats == NULL)
	{
		fprintf(stderr, "V4L2_CORE: FATAL memory allocation failure (add_h264_format): %s\n", strerror(errno));
		exit(-1);
	}

	vd->list_stream_formats[fmtind-1].format = V4L2_PIX_FMT_H264;
	vd->list_stream_formats[fmtind-1].dec_support = 1;
	snprintf(vd->list_stream_formats[fmtind-1].fourcc , 5, "H264");
	vd->list_stream_formats[fmtind-1].list_stream_cap = NULL;
	vd->list_stream_formats[fmtind-1].numb_res = 0;


	/*add MJPG resolutions and frame rates for H264*/
	int numb_res = vd->list_stream_formats[mjpg_index].numb_res;

	int i=0, j=0;
	int res_index = 0;
	for(i=0; i < numb_res; i++)
	{
		int width = vd->list_stream_formats[mjpg_index].list_stream_cap[i].width;
		int height = vd->list_stream_formats[mjpg_index].list_stream_cap[i].height;

		res_index++;
		vd->list_stream_formats[fmtind-1].list_stream_cap = realloc(
			vd->list_stream_formats[fmtind-1].list_stream_cap,
            (size_t)res_index * sizeof(v4l2_stream_cap_t));
		if(vd->list_stream_formats[fmtind-1].list_stream_cap == NULL)
		{
			fprintf(stderr, "V4L2_CORE: FATAL memory allocation failure (add_h264_format): %s\n", strerror(errno));
			exit(-1);
		}
		vd->list_stream_formats[fmtind-1].numb_res = res_index;
		vd->list_stream_formats[fmtind-1].list_stream_cap[res_index-1].width = width;
		vd->list_stream_formats[fmtind-1].list_stream_cap[res_index-1].height = height;
		vd->list_stream_formats[fmtind-1].list_stream_cap[res_index-1].framerate_num = NULL;
		vd->list_stream_formats[fmtind-1].list_stream_cap[res_index-1].framerate_denom = NULL;
		vd->list_stream_formats[fmtind-1].list_stream_cap[res_index-1].numb_frates = 0;

		/*add frates*/
		int numb_frates =  vd->list_stream_formats[mjpg_index].list_stream_cap[i].numb_frates;
		int frate_index = 0;
		for(j=0; j < numb_frates; j++)
		{
			int framerate_num = vd->list_stream_formats[mjpg_index].list_stream_cap[i].framerate_num[j];
			int framerate_denom = vd->list_stream_formats[mjpg_index].list_stream_cap[i].framerate_denom[j];

			frate_index++;
			vd->list_stream_formats[fmtind-1].list_stream_cap[res_index-1].numb_frates = frate_index;
			vd->list_stream_formats[fmtind-1].list_stream_cap[res_index-1].framerate_num = realloc(
				vd->list_stream_formats[fmtind-1].list_stream_cap[res_index-1].framerate_num,
                (size_t)frate_index * sizeof(int));
			if(vd->list_stream_formats[fmtind-1].list_stream_cap[res_index-1].framerate_num == NULL)
			{
				fprintf(stderr, "V4L2_CORE: FATAL memory allocation failure (add_h264_format): %s\n", strerror(errno));
				exit(-1);
			}

			vd->list_stream_formats[fmtind-1].list_stream_cap[res_index-1].framerate_num[frate_index-1] = framerate_num;
			vd->list_stream_formats[fmtind-1].list_stream_cap[res_index-1].framerate_denom = realloc(
				vd->list_stream_formats[fmtind-1].list_stream_cap[res_index-1].framerate_denom,
                (size_t)frate_index * sizeof(int));
			if(vd->list_stream_formats[fmtind-1].list_stream_cap[res_index-1].framerate_denom == NULL)
			{
				fprintf(stderr, "V4L2_CORE: FATAL memory allocation failure (add_h264_format): %s\n", strerror(errno));
				exit(-1);
			}
			vd->list_stream_formats[fmtind-1].list_stream_cap[res_index-1].framerate_denom[frate_index-1] = framerate_denom;
		}
	}
}

/*
 * sets h264 muxed format (must not be called while streaming)
 * args:
 *   vd - pointer to video device data
 *
 * asserts:
 *   vd is not null
 *
 * returns: void
 */
void set_h264_muxed_format(v4l2_dev_t *vd)
{
	uvcx_video_config_probe_commit_t *config_probe_req = &(vd->h264_config_probe_req);

	/* reset the encoder*/
	uvcx_video_encoder_reset(vd);

	/*
	 * Get default values (safe)
	 */
	if(!(vd->h264_no_probe_default))
		uvcx_video_probe(vd, UVC_GET_DEF, config_probe_req);

	/*reset it - must be set on every call*/
	vd->h264_no_probe_default = 0;

	/*set resolution*/
    config_probe_req->wWidth = (uint16_t)vd->format.fmt.pix.width;
    config_probe_req->wHeight = (uint16_t)vd->format.fmt.pix.height;
	/*set frame rate in 100ns units*/
    uint32_t frame_interval = ((uint32_t)vd->fps_num * 1000000000LL / (uint32_t)vd->fps_denom)/100;
	config_probe_req->dwFrameInterval = frame_interval;

	/*set the aux stream (h264)*/
	config_probe_req->bStreamMuxOption = STREAMMUX_H264;

	/*probe the format*/
	uvcx_video_probe(vd, UVC_SET_CUR, config_probe_req);
	uvcx_video_probe(vd, UVC_GET_CUR, config_probe_req);

	if(config_probe_req->wWidth != vd->format.fmt.pix.width)
	{
		fprintf(stderr, "V4L2_CORE: H264 config probe: requested width %i but got %i\n",
			vd->format.fmt.pix.width, config_probe_req->wWidth);

		vd->format.fmt.pix.width = config_probe_req->wWidth;
	}
	if(config_probe_req->wHeight != vd->format.fmt.pix.height)
	{
		fprintf(stderr, "V4L2_CORE: H264 config probe: requested height %i but got %i\n",
			vd->format.fmt.pix.height, config_probe_req->wHeight);

		vd->format.fmt.pix.height = config_probe_req->wHeight;
	}
	if(config_probe_req->dwFrameInterval != frame_interval)
	{
        fprintf(stderr, "V4L2_CORE: H264 config probe: requested frame interval %u but got %u\n",
			frame_interval, config_probe_req->dwFrameInterval);
	}
	/*commit the format*/
	uvcx_video_commit(vd, config_probe_req);

	/*print probe/commit data*/
	if(verbosity > 0)
		print_probe_commit_data(config_probe_req);
}

/*
 * request a frame of type wPictureType to the h264 encoder
 * args:
 *   vd - pointer to video device data
 *   type - frame type
 *
 * asserts:
 *   vd is not null
 *
 * returns: error code
 */
int request_h264_frame_type(v4l2_dev_t *vd, uint16_t type)
{
	/*asserts*/
	assert(vd != NULL);

	if(vd->h264_unit_id <= 0)
	{
		if(verbosity > 0)
			printf("V4L2_CORE: device doesn't seem to support uvc H264 (%i)\n", vd->h264_unit_id);
		return E_NO_STREAM_ERR;
	}

	uvcx_picture_type_control_t picture_type_req;
	picture_type_req.wLayerID = 0;
	picture_type_req.wPicType = type;

	int err = E_OK;

	if((err = v4l2core_query_xu_control(
		vd,
		vd->h264_unit_id,
		UVCX_PICTURE_TYPE_CONTROL,
		UVC_SET_CUR,
		&picture_type_req)) < 0)
	{
		fprintf(stderr, "V4L2_CORE: (UVCX_PICTURE_TYPE_CONTROL) SET_CUR error: %s\n", strerror(errno));
	}

	return err;
}

/*
 * resets the h264 encoder
 * args:
 *   vd - pointer to video device data
 *
 * asserts:
 *   vd is not null
 *
 * returns: 0 on success or error code on fail
 */
int h264_reset_encoder(v4l2_dev_t *vd)
{
	/*asserts*/
	assert(vd != NULL);

	/* reset the encoder*/
	return uvcx_video_encoder_reset(vd);
}

/*
 * get the video rate control mode
 * args:
 *   vd - pointer to video device data
 *   query - query type
 *
 * asserts:
 *   vd is not null
 *
 * returns: video rate control mode (FIXME: 0xff on error)
 */
uint8_t  h264_get_video_rate_control_mode(v4l2_dev_t *vd, uint8_t query)
{
	/*asserts*/
	assert(vd != NULL);

	if(vd->h264_unit_id <= 0)
	{
		if(verbosity > 0)
			printf("V4L2_CORE: device doesn't seem to support uvc H264 (%i)\n", vd->h264_unit_id);
		return 0xff;
	}

	uvcx_rate_control_mode_t rate_control_mode_req;
	rate_control_mode_req.wLayerID = 0;

	if((v4l2core_query_xu_control(
		vd,
		vd->h264_unit_id,
		UVCX_RATE_CONTROL_MODE,
		query,
		&rate_control_mode_req)) < 0)
	{
		fprintf(stderr, "V4L2_CORE: (UVCX_RATE_CONTROL_MODE) query (%u) error: %s\n", query, strerror(errno));
		return 0xff;
	}

	return rate_control_mode_req.bRateControlMode;
}

/*
 * set the video rate control mode
 * args:
 *   vd - pointer to video device data
 *   mode - rate mode
 *
 * asserts:
 *   vd is not null
 *
 * returns: error code ( 0 -OK)
 */
int h264_set_video_rate_control_mode(v4l2_dev_t *vd, uint8_t mode)
{
	/*asserts*/
	assert(vd != NULL);

	if(vd->h264_unit_id <= 0)
	{
		if(verbosity > 0)
			printf("V4L2_CORE: device doesn't seem to support uvc H264 (%i)\n", vd->h264_unit_id);
		return E_NO_STREAM_ERR;
	}

	uvcx_rate_control_mode_t rate_control_mode_req;
	rate_control_mode_req.wLayerID = 0;
	rate_control_mode_req.bRateControlMode = mode;

	int err = E_OK;

	if((err = v4l2core_query_xu_control(
		vd,
		vd->h264_unit_id,
		UVCX_RATE_CONTROL_MODE,
		UVC_SET_CUR,
		&rate_control_mode_req)) < 0)
	{
		fprintf(stderr, "V4L2_CORE: (UVCX_RATE_CONTROL_MODE) SET_CUR error: %s\n", strerror(errno));
	}

	return err;
}

/*
 * get the temporal scale mode
 * args:
 *   vd - pointer to video device data
 *   query - query type
 *
 * asserts:
 *   vd is not null
 *
 * returns: temporal scale mode (FIXME: 0xff on error)
 */
uint8_t h264_get_temporal_scale_mode(v4l2_dev_t *vd, uint8_t query)
{
	/*asserts*/
	assert(vd != NULL);

	if(vd->h264_unit_id <= 0)
	{
		if(verbosity > 0)
			printf("V4L2_CORE: device doesn't seem to support uvc H264 (%i)\n", vd->h264_unit_id);
		return 0xff;
	}

	uvcx_temporal_scale_mode_t temporal_scale_mode_req;
	temporal_scale_mode_req.wLayerID = 0;

	if(v4l2core_query_xu_control(
		vd,
		vd->h264_unit_id,
		UVCX_TEMPORAL_SCALE_MODE,
		query,
		&temporal_scale_mode_req) < 0)
	{
		fprintf(stderr, "V4L2_CORE: (UVCX_TEMPORAL_SCALE_MODE) query (%u) error: %s\n", query, strerror(errno));
		return 0xff;
	}

	return temporal_scale_mode_req.bTemporalScaleMode;
}

/*
 * set the temporal scale mode
 * args:
 *   vd - pointer to video device data
 *   mode - temporal scale mode
 *
 * asserts:
 *   vd is not null
 *
 * returns: error code ( 0 -OK)
 */
int h264_set_temporal_scale_mode(v4l2_dev_t *vd, uint8_t mode)
{
	/*asserts*/
	assert(vd != NULL);

	if(vd->h264_unit_id <= 0)
	{
		if(verbosity > 0)
			printf("V4L2_CORE: device doesn't seem to support uvc H264 (%i)\n", vd->h264_unit_id);
		return E_NO_STREAM_ERR;
	}

	uvcx_temporal_scale_mode_t temporal_scale_mode_req;
	temporal_scale_mode_req.wLayerID = 0;
	temporal_scale_mode_req.bTemporalScaleMode = mode;

	int err = 0;

	if((err = v4l2core_query_xu_control(
		vd,
		vd->h264_unit_id,
		UVCX_TEMPORAL_SCALE_MODE,
		UVC_SET_CUR,
		&temporal_scale_mode_req)) < 0)
	{
		fprintf(stderr, "V4L2_CORE: (UVCX_TEMPORAL_SCALE_MODE) SET_CUR error: %s\n", strerror(errno));
	}

	return err;
}

/*
 * get the spatial scale mode
 * args:
 *   vd - pointer to video device data
 *   query - query type
 *
 * asserts:
 *   vd is not null
 *
 * returns: temporal scale mode (FIXME: 0xff on error)
 */
uint8_t h264_get_spatial_scale_mode(v4l2_dev_t *vd, uint8_t query)
{
	/*asserts*/
	assert(vd != NULL);

	if(vd->h264_unit_id <= 0)
	{
		if(verbosity > 0)
			printf("V4L2_CORE: device doesn't seem to support uvc H264 (%i)\n", vd->h264_unit_id);
		return 0xff;
	}

	uvcx_spatial_scale_mode_t spatial_scale_mode_req;
	spatial_scale_mode_req.wLayerID = 0;

	if(v4l2core_query_xu_control(
		vd,
		vd->h264_unit_id,
		UVCX_SPATIAL_SCALE_MODE,
		query,
		&spatial_scale_mode_req) < 0)
	{
		fprintf(stderr, "V4L2_CORE: (UVCX_SPATIAL_SCALE_MODE) query (%u) error: %s\n", query, strerror(errno));
		return 0xff;
	}

	return spatial_scale_mode_req.bSpatialScaleMode;
}

/*
 * set the spatial scale mode
 * args:
 *   vd - pointer to video device data
 *   mode - spatial scale mode
 *
 * asserts:
 *   vd is not null
 *
 * returns: error code ( 0 -OK)
 */
int h264_set_spatial_scale_mode(v4l2_dev_t *vd, uint8_t mode)
{
	/*asserts*/
	assert(vd != NULL);

	if(vd->h264_unit_id <= 0)
	{
		if(verbosity > 0)
			printf("V4L2_CORE: device doesn't seem to support uvc H264 (%i)\n", vd->h264_unit_id);
		return E_NO_STREAM_ERR;
	}

	uvcx_spatial_scale_mode_t spatial_scale_mode_req;
	spatial_scale_mode_req.wLayerID = 0;
	spatial_scale_mode_req.bSpatialScaleMode = mode;

	int err = 0;

	if((err = v4l2core_query_xu_control(
		vd,
		vd->h264_unit_id,
		UVCX_SPATIAL_SCALE_MODE,
		UVC_SET_CUR,
		&spatial_scale_mode_req)) < 0)
	{
		fprintf(stderr, "V4L2_CORE: (UVCX_SPATIAL_SCALE_MODE) SET_CUR error: %s\n", strerror(errno));
	}

	return err;
}

/*
 * query the frame rate config
 * args:
 *   vd - pointer to video device data
 *   query - query type
 *
 * asserts:
 *   vd is not null
 *
 * returns: frame rate config (FIXME: 0xffffffff on error)
 */
uint32_t h264_query_frame_rate_config(v4l2_dev_t *vd, uint8_t query)
{
	/*asserts*/
	assert(vd != NULL);

	if(vd->h264_unit_id <= 0)
	{
		if(verbosity > 0)
			printf("V4L2_CORE: device doesn't seem to support uvc H264 (%i)\n", vd->h264_unit_id);
		return 0xffffffff;
	}

	uvcx_framerate_config_t framerate_req;
	framerate_req.wLayerID = 0;

	if(v4l2core_query_xu_control(
		vd,
		vd->h264_unit_id,
		UVCX_FRAMERATE_CONFIG,
		query,
		&framerate_req) < 0)
	{
		fprintf(stderr, "V4L2_CORE: (UVCX_FRAMERATE_CONFIG) query (%u) error: %s\n", query, strerror(errno));
		return 0xffffffff;
	}

	return framerate_req.dwFrameInterval;
}

/*
 * get the frame rate config
 * args:
 *   vd - pointer to video device data
 *
 * asserts:
 *   vd is not null
 *
 * returns: frame rate config (FIXME: 0xffffffff on error)
 */
uint32_t h264_get_frame_rate_config(v4l2_dev_t *vd)
{
	return h264_query_frame_rate_config(vd, UVC_GET_CUR);
}

/*
 * set the frame rate config
 * args:
 *   vd - pointer to video device data
 *   framerate - framerate
 *
 * asserts:
 *   vd is not null
 *
 * returns: error code ( 0 -OK)
 */
int h264_set_frame_rate_config(v4l2_dev_t *vd, uint32_t framerate)
{
	/*asserts*/
	assert(vd != NULL);

	if(vd->h264_unit_id <= 0)
	{
		if(verbosity > 0)
			printf("V4L2_CORE: device doesn't seem to support uvc H264 (%i)\n", vd->h264_unit_id);
		return E_NO_STREAM_ERR;
	}

	uvcx_framerate_config_t framerate_req;
	framerate_req.wLayerID = 0;
	framerate_req.dwFrameInterval = framerate;

	int err = 0;

	if((err = v4l2core_query_xu_control(
		vd,
		vd->h264_unit_id,
		UVCX_FRAMERATE_CONFIG,
		UVC_SET_CUR,
		&framerate_req)) < 0)
	{
		fprintf(stderr, "V4L2_CORE: (UVCX_FRAMERATE_CONFIG) SET_CUR error: %s\n", strerror(errno));
	}

	return err;
}

/*
 * updates the h264_probe_commit_req field
 * args:
 *   vd - pointer to video device data
 *   query - (UVC_GET_CUR; UVC_GET_MAX; UVC_GET_MIN)
 *   config_probe_cur - pointer to uvcx_video_config_probe_commit_t:
 *     if null vd->h264_config_probe_req will be used
 *
 * asserts:
 *   vd is not null
 *
 * returns: error code ( 0 -OK)
 */
int h264_probe_config_probe_req(
			v4l2_dev_t *vd,
			uint8_t query,
			uvcx_video_config_probe_commit_t *config_probe_req)
{
	/*asserts*/
	assert(vd != NULL);

	if(vd->h264_unit_id <= 0)
	{
		if(verbosity > 0)
			printf("V4L2_CORE: device doesn't seem to support uvc H264 (%i)\n", vd->h264_unit_id);
		return E_NO_STREAM_ERR;
	}

	if(config_probe_req == NULL)
		config_probe_req = &(vd->h264_config_probe_req);

	return uvcx_video_probe(vd, query, config_probe_req);
}

/*
 * ############# H264 decoder ##############
 */

/* 把带行跨度的平面紧凑拷贝到 YUV420P 输出缓冲 */
static void h264_copy_plane(uint8_t *dst, const uint8_t *src,
	int width, int height, int stride)
{
	int y = 0;
	for (y = 0; y < height; ++y)
		memcpy(dst + (size_t)y * width, src + (size_t)y * stride, (size_t)width);
}

/* ucodec RAM 输出回调：解码器输出生命周期仅限回调内，立即拷出 */
static void h264_frame_cb(const ucodec_frame *frame, void *user)
{
	h264_decoder_context_t *ctx = (h264_decoder_context_t *)user;
	int width = 0, height = 0, cw = 0, ch = 0;

	if (ctx == NULL || frame == NULL || ctx->out_buf == NULL)
		return;

	width = ctx->width;
	height = ctx->height;
	cw = (width + 1) / 2;
	ch = (height + 1) / 2;

	if (frame->width != width || frame->height != height)
	{
		fprintf(stderr, "V4L2_CORE: (H264 decoder) size mismatch: frame %ix%i, buffer %ix%i\n",
			frame->width, frame->height, width, height);
		return;
	}

	switch (frame->format)
	{
		case UCODEC_FMT_YUV420P:
			if (frame->data[0] == NULL || frame->data[1] == NULL || frame->data[2] == NULL)
				return;
			h264_copy_plane(ctx->out_buf, frame->data[0], width, height, frame->linesize[0]);
			h264_copy_plane(ctx->out_buf + (size_t)width * height, frame->data[1],
				cw, ch, frame->linesize[1]);
			h264_copy_plane(ctx->out_buf + (size_t)width * height + (size_t)cw * ch,
				frame->data[2], cw, ch, frame->linesize[2]);
			break;

		case UCODEC_FMT_NV12:
		{
			int y = 0, x = 0;
			uint8_t *u = ctx->out_buf + (size_t)width * height;
			uint8_t *v = u + (size_t)cw * ch;

			h264_copy_plane(ctx->out_buf, frame->data[0], width, height, frame->linesize[0]);
			for (y = 0; y < ch; ++y)
			{
				const uint8_t *row = frame->data[1] + (size_t)y * frame->linesize[1];
				for (x = 0; x < cw; ++x)
				{
					u[(size_t)y * cw + x] = row[x * 2];
					v[(size_t)y * cw + x] = row[x * 2 + 1];
				}
			}
			break;
		}

		case UCODEC_FMT_P010: /* 2 平面，10 bit 位于高 10 位 */
		{
			int y = 0, x = 0;
			uint8_t *u = ctx->out_buf + (size_t)width * height;
			uint8_t *v = u + (size_t)cw * ch;
			const uint16_t *srcY = (const uint16_t *)frame->data[0];
			const uint16_t *srcUV = (const uint16_t *)frame->data[1];
			int strideY = frame->linesize[0] / 2;
			int strideUV = frame->linesize[1] / 2;

			if (frame->data[1] == NULL)
				return;

			for (y = 0; y < height; ++y)
				for (x = 0; x < width; ++x)
					ctx->out_buf[(size_t)y * width + x] =
						(uint8_t)(srcY[(size_t)y * strideY + x] >> 8);
			for (y = 0; y < ch; ++y)
			{
				const uint16_t *row = srcUV + (size_t)y * strideUV;
				for (x = 0; x < cw; ++x)
				{
					u[(size_t)y * cw + x] = (uint8_t)(row[x * 2] >> 8);
					v[(size_t)y * cw + x] = (uint8_t)(row[x * 2 + 1] >> 8);
				}
			}
			break;
		}

		case UCODEC_FMT_YUV420P10: /* 3 平面，10 bit 位于低 10 位 */
		{
			int y = 0, x = 0;
			uint8_t *u = ctx->out_buf + (size_t)width * height;
			uint8_t *v = u + (size_t)cw * ch;
			const uint16_t *srcY = (const uint16_t *)frame->data[0];
			const uint16_t *srcU = (const uint16_t *)frame->data[1];
			const uint16_t *srcV = (const uint16_t *)frame->data[2];
			int strideY = frame->linesize[0] / 2;
			int strideU = frame->linesize[1] / 2;
			int strideV = frame->linesize[2] / 2;

			if (frame->data[1] == NULL || frame->data[2] == NULL)
				return;

			for (y = 0; y < height; ++y)
				for (x = 0; x < width; ++x)
					ctx->out_buf[(size_t)y * width + x] =
						(uint8_t)(srcY[(size_t)y * strideY + x] >> 2);
			for (y = 0; y < ch; ++y)
				for (x = 0; x < cw; ++x)
				{
					u[(size_t)y * cw + x] = (uint8_t)(srcU[(size_t)y * strideU + x] >> 2);
					v[(size_t)y * cw + x] = (uint8_t)(srcV[(size_t)y * strideV + x] >> 2);
				}
			break;
		}

		default:
			fprintf(stderr, "V4L2_CORE: (H264 decoder) unsupported output format %i\n",
				frame->format);
			return;
	}

	ctx->got_frame = 1;
}

/*
 * init h264 decoder context (ucodec software/hardware decoder)
 * args:
 *    width - image width
 *    height - image height
 *
 * asserts:
 *    none
 *
 * returns: error code (0 - E_OK)
 */
int h264_init_decoder(int width, int height)
{
	ucodec_dec_config config;
	ucodec_status status;

	if(h264_ctx != NULL)
		h264_close_decoder();

	h264_ctx = calloc(1, sizeof(h264_decoder_context_t));
	if(h264_ctx == NULL)
	{
		fprintf(stderr, "V4L2_CORE: FATAL memory allocation failure (h264_init_decoder): %s\n", strerror(errno));
		exit(-1);
	}

	memset(&config, 0, sizeof(config));
	config.codec = "h264";
	config.prefer_hw = 1;
	config.allow_sw_fallback = 1;
	config.output = UCODEC_OUTPUT_RAM;
	config.packet_format = UCODEC_PKTS_ANNEXB;

	status = ucodec_decoder_create(&config, &h264_ctx->decoder);
	if(status != UCODEC_OK || h264_ctx->decoder == NULL)
	{
		fprintf(stderr, "V4L2_CORE: (H264 decoder) ucodec_decoder_create failed: %i\n",
			(int)status);
		free(h264_ctx);
		h264_ctx = NULL;
		return E_NO_CODEC;
	}

	ucodec_decoder_set_ram_cb(h264_ctx->decoder, h264_frame_cb, h264_ctx);

	h264_ctx->width = width;
	h264_ctx->height = height;
	h264_ctx->pic_size = width * height * 3 / 2;

	return E_OK;
}

/*
 * decode h264 frame
 * args:
 *    out_buf - pointer to decoded data (tight YUV420P)
 *    in_buf - pointer to h264 data (Annex-B access unit)
 *    size - in_buf size
 *
 * asserts:
 *    h264_ctx is not null
 *    in_buf is not null
 *    out_buf is not null
 *
 * returns: decoded data size (0 when no frame was produced)
 */
int h264_decode(uint8_t *out_buf, uint8_t *in_buf, int size)
{
	ucodec_status status;

	/*asserts*/
	assert(h264_ctx != NULL);
	assert(in_buf != NULL);
	assert(out_buf != NULL);

	h264_ctx->out_buf = out_buf;
	h264_ctx->out_size = h264_ctx->pic_size;
	h264_ctx->got_frame = 0;

	status = ucodec_decoder_decode(h264_ctx->decoder, in_buf, size, 0);
	if(status != UCODEC_OK)
	{
		fprintf(stderr, "V4L2_CORE: (H264 decoder) error while decoding frame: %i\n",
			(int)status);
		return -1;
	}

	h264_ctx->out_buf = NULL;
	return h264_ctx->got_frame ? h264_ctx->pic_size : 0;
}

/*
 * close h264 decoder context
 * args:
 *    none
 *
 * asserts:
 *    none
 *
 * returns: none
 */
void h264_close_decoder()
{
	if(h264_ctx == NULL)
		return;

	if(h264_ctx->decoder != NULL)
		ucodec_decoder_destroy(h264_ctx->decoder);

	free(h264_ctx);

	h264_ctx = NULL;
}
