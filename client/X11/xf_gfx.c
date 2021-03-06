/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * X11 Graphics Pipeline
 *
 * Copyright 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <freerdp/log.h>
#include "xf_gfx.h"

#define TAG CLIENT_TAG("x11")

int xf_ResetGraphics(RdpgfxClientContext* context, RDPGFX_RESET_GRAPHICS_PDU* resetGraphics)
{
	xfContext* xfc = (xfContext*) context->custom;

	freerdp_client_codecs_reset(xfc->codecs, FREERDP_CODEC_ALL);

	region16_uninit(&(xfc->invalidRegion));
	region16_init(&(xfc->invalidRegion));

	xfc->graphicsReset = TRUE;

	return 1;
}

int xf_OutputUpdate(xfContext* xfc)
{
	UINT16 width, height;
	xfGfxSurface* surface;
	RECTANGLE_16 surfaceRect;
	const RECTANGLE_16* extents;

	if (!xfc->graphicsReset)
		return 1;

	surface = (xfGfxSurface*) xfc->gfx->GetSurfaceData(xfc->gfx, xfc->outputSurfaceId);

	if (!surface)
		return -1;

	surfaceRect.left = 0;
	surfaceRect.top = 0;
	surfaceRect.right = xfc->width;
	surfaceRect.bottom = xfc->height;

	region16_intersect_rect(&(xfc->invalidRegion), &(xfc->invalidRegion), &surfaceRect);

	XSetClipMask(xfc->display, xfc->gc, None);
	XSetFunction(xfc->display, xfc->gc, GXcopy);
	XSetFillStyle(xfc->display, xfc->gc, FillSolid);

	if (!region16_is_empty(&(xfc->invalidRegion)))
	{
		extents = region16_extents(&(xfc->invalidRegion));

		width = extents->right - extents->left;
		height = extents->bottom - extents->top;

		if (width > xfc->width)
			width = xfc->width;

		if (height > xfc->height)
			height = xfc->height;

		if (surface->stage)
		{
			freerdp_image_copy(surface->stage, xfc->format, surface->stageStep, 0, 0,
				surface->width, surface->height, surface->data, surface->format, surface->scanline, 0, 0, NULL);
		}

#ifdef WITH_XRENDER
		if (xfc->settings->SmartSizing || xfc->settings->MultiTouchGestures)
		{
			XPutImage(xfc->display, xfc->primary, xfc->gc, surface->image,
				extents->left, extents->top, extents->left, extents->top, width, height);

			xf_draw_screen(xfc, extents->left, extents->top, width, height);
		}
		else
#endif
		{
			XPutImage(xfc->display, xfc->drawable, xfc->gc, surface->image,
				extents->left, extents->top, extents->left, extents->top, width, height);
		}
	}

	region16_clear(&(xfc->invalidRegion));

	XSetClipMask(xfc->display, xfc->gc, None);
	XSync(xfc->display, True);

	return 1;
}

int xf_OutputExpose(xfContext* xfc, int x, int y, int width, int height)
{
	RECTANGLE_16 invalidRect;

	invalidRect.left = x;
	invalidRect.top = y;
	invalidRect.right = x + width;
	invalidRect.bottom = y + height;

	region16_union_rect(&(xfc->invalidRegion), &(xfc->invalidRegion), &invalidRect);

	xf_OutputUpdate(xfc);

	return 1;
}

int xf_StartFrame(RdpgfxClientContext* context, RDPGFX_START_FRAME_PDU* startFrame)
{
	xfContext* xfc = (xfContext*) context->custom;

	xfc->inGfxFrame = TRUE;

	return 1;
}

int xf_EndFrame(RdpgfxClientContext* context, RDPGFX_END_FRAME_PDU* endFrame)
{
	xfContext* xfc = (xfContext*) context->custom;

	xf_OutputUpdate(xfc);

	xfc->inGfxFrame = FALSE;

	return 1;
}

int xf_SurfaceCommand_Uncompressed(xfContext* xfc, RdpgfxClientContext* context, RDPGFX_SURFACE_COMMAND* cmd)
{
	xfGfxSurface* surface;
	RECTANGLE_16 invalidRect;

	surface = (xfGfxSurface*) context->GetSurfaceData(context, cmd->surfaceId);

	if (!surface)
		return -1;

	freerdp_image_copy(surface->data, surface->format, surface->scanline, cmd->left, cmd->top,
			cmd->width, cmd->height, cmd->data, PIXEL_FORMAT_XRGB32, -1, 0, 0, NULL);

	invalidRect.left = cmd->left;
	invalidRect.top = cmd->top;
	invalidRect.right = cmd->right;
	invalidRect.bottom = cmd->bottom;

	region16_union_rect(&(xfc->invalidRegion), &(xfc->invalidRegion), &invalidRect);

	if (!xfc->inGfxFrame)
		xf_OutputUpdate(xfc);

	return 1;
}

int xf_SurfaceCommand_RemoteFX(xfContext* xfc, RdpgfxClientContext* context, RDPGFX_SURFACE_COMMAND* cmd)
{
	int j;
	UINT16 i;
	RFX_RECT* rect;
	RFX_TILE* tile;
	int nXDst, nYDst;
	int nWidth, nHeight;
	int nbUpdateRects;
	RFX_MESSAGE* message;
	xfGfxSurface* surface;
	REGION16 updateRegion;
	RECTANGLE_16 updateRect;
	RECTANGLE_16* updateRects;
	REGION16 clippingRects;
	RECTANGLE_16 clippingRect;

	freerdp_client_codecs_prepare(xfc->codecs, FREERDP_CODEC_REMOTEFX);

	surface = (xfGfxSurface*) context->GetSurfaceData(context, cmd->surfaceId);

	if (!surface)
		return -1;

	message = rfx_process_message(xfc->codecs->rfx, cmd->data, cmd->length);

	if (!message)
		return -1;

	region16_init(&clippingRects);

	for (i = 0; i < message->numRects; i++)
	{
		rect = &(message->rects[i]);

		clippingRect.left = cmd->left + rect->x;
		clippingRect.top = cmd->top + rect->y;
		clippingRect.right = clippingRect.left + rect->width;
		clippingRect.bottom = clippingRect.top + rect->height;

		region16_union_rect(&clippingRects, &clippingRects, &clippingRect);
	}

	for (i = 0; i < message->numTiles; i++)
	{
		tile = message->tiles[i];

		updateRect.left = cmd->left + tile->x;
		updateRect.top = cmd->top + tile->y;
		updateRect.right = updateRect.left + 64;
		updateRect.bottom = updateRect.top + 64;

		region16_init(&updateRegion);
		region16_intersect_rect(&updateRegion, &clippingRects, &updateRect);
		updateRects = (RECTANGLE_16*) region16_rects(&updateRegion, &nbUpdateRects);

		for (j = 0; j < nbUpdateRects; j++)
		{
			nXDst = updateRects[j].left;
			nYDst = updateRects[j].top;
			nWidth = updateRects[j].right - updateRects[j].left;
			nHeight = updateRects[j].bottom - updateRects[j].top;

			freerdp_image_copy(surface->data, surface->format, surface->scanline,
					nXDst, nYDst, nWidth, nHeight,
					tile->data, PIXEL_FORMAT_XRGB32, 64 * 4, 0, 0, NULL);

			region16_union_rect(&(xfc->invalidRegion), &(xfc->invalidRegion), &updateRects[j]);
		}

		region16_uninit(&updateRegion);
	}

	rfx_message_free(xfc->codecs->rfx, message);

	region16_uninit(&clippingRects);

	if (!xfc->inGfxFrame)
		xf_OutputUpdate(xfc);

	return 1;
}

int xf_SurfaceCommand_ClearCodec(xfContext* xfc, RdpgfxClientContext* context, RDPGFX_SURFACE_COMMAND* cmd)
{
	int status;
	BYTE* DstData = NULL;
	xfGfxSurface* surface;
	RECTANGLE_16 invalidRect;

	freerdp_client_codecs_prepare(xfc->codecs, FREERDP_CODEC_CLEARCODEC);

	surface = (xfGfxSurface*) context->GetSurfaceData(context, cmd->surfaceId);

	if (!surface)
		return -1;

	DstData = surface->data;

	status = clear_decompress(xfc->codecs->clear, cmd->data, cmd->length, &DstData,
			surface->format, surface->scanline, cmd->left, cmd->top, cmd->width, cmd->height);

	if (status < 0)
	{
		WLog_ERR(TAG, "clear_decompress failure: %d\n", status);
		return -1;
	}

	invalidRect.left = cmd->left;
	invalidRect.top = cmd->top;
	invalidRect.right = cmd->right;
	invalidRect.bottom = cmd->bottom;

	region16_union_rect(&(xfc->invalidRegion), &(xfc->invalidRegion), &invalidRect);

	if (!xfc->inGfxFrame)
		xf_OutputUpdate(xfc);

	return 1;
}

int xf_SurfaceCommand_Planar(xfContext* xfc, RdpgfxClientContext* context, RDPGFX_SURFACE_COMMAND* cmd)
{
	int status;
	BYTE* DstData = NULL;
	xfGfxSurface* surface;
	RECTANGLE_16 invalidRect;

	freerdp_client_codecs_prepare(xfc->codecs, FREERDP_CODEC_PLANAR);

	surface = (xfGfxSurface*) context->GetSurfaceData(context, cmd->surfaceId);

	if (!surface)
		return -1;

	DstData = surface->data;

	status = planar_decompress(xfc->codecs->planar, cmd->data, cmd->length, &DstData,
			surface->format, surface->scanline, cmd->left, cmd->top, cmd->width, cmd->height, FALSE);

	invalidRect.left = cmd->left;
	invalidRect.top = cmd->top;
	invalidRect.right = cmd->right;
	invalidRect.bottom = cmd->bottom;

	region16_union_rect(&(xfc->invalidRegion), &(xfc->invalidRegion), &invalidRect);

	if (!xfc->inGfxFrame)
		xf_OutputUpdate(xfc);

	return 1;
}

int xf_SurfaceCommand_H264(xfContext* xfc, RdpgfxClientContext* context, RDPGFX_SURFACE_COMMAND* cmd)
{
	int status;
	UINT32 i;
	BYTE* DstData = NULL;
	H264_CONTEXT* h264;
	xfGfxSurface* surface;
	RDPGFX_H264_METABLOCK* meta;
	RDPGFX_H264_BITMAP_STREAM* bs;

	freerdp_client_codecs_prepare(xfc->codecs, FREERDP_CODEC_H264);

	h264 = xfc->codecs->h264;

	bs = (RDPGFX_H264_BITMAP_STREAM*) cmd->extra;

	if (!bs)
		return -1;

	meta = &(bs->meta);

	surface = (xfGfxSurface*) context->GetSurfaceData(context, cmd->surfaceId);

	if (!surface)
		return -1;

	DstData = surface->data;

	status = h264_decompress(xfc->codecs->h264, bs->data, bs->length, &DstData,
			surface->format, surface->scanline , surface->height, meta->regionRects, meta->numRegionRects);

	if (status < 0)
	{
		WLog_ERR(TAG, "h264_decompress failure: %d",status);
		return -1;
	}

	for (i = 0; i < meta->numRegionRects; i++)
	{
		region16_union_rect(&(xfc->invalidRegion), &(xfc->invalidRegion), (RECTANGLE_16*) &(meta->regionRects[i]));
	}

	if (!xfc->inGfxFrame)
		xf_OutputUpdate(xfc);

	return 1;
}

int xf_SurfaceCommand_Alpha(xfContext* xfc, RdpgfxClientContext* context, RDPGFX_SURFACE_COMMAND* cmd)
{
	int status = 0;
	xfGfxSurface* surface;
	RECTANGLE_16 invalidRect;

	freerdp_client_codecs_prepare(xfc->codecs, FREERDP_CODEC_ALPHACODEC);

	surface = (xfGfxSurface*) context->GetSurfaceData(context, cmd->surfaceId);

	if (!surface)
		return -1;

	WLog_DBG(TAG, "xf_SurfaceCommand_Alpha: status: %d\n", status);
	/* fill with green for now to distinguish from the rest */

	freerdp_image_fill(surface->data, PIXEL_FORMAT_XRGB32, surface->scanline,
			cmd->left, cmd->top, cmd->width, cmd->height, 0x00FF00);

	invalidRect.left = cmd->left;
	invalidRect.top = cmd->top;
	invalidRect.right = cmd->right;
	invalidRect.bottom = cmd->bottom;

	region16_union_rect(&(xfc->invalidRegion), &(xfc->invalidRegion), &invalidRect);

	if (!xfc->inGfxFrame)
		xf_OutputUpdate(xfc);

	return 1;
}

int xf_SurfaceCommand_Progressive(xfContext* xfc, RdpgfxClientContext* context, RDPGFX_SURFACE_COMMAND* cmd)
{
	int i, j;
	int status;
	BYTE* DstData;
	RFX_RECT* rect;
	int nXDst, nYDst;
	int nXSrc, nYSrc;
	int nWidth, nHeight;
	int nbUpdateRects;
	xfGfxSurface* surface;
	REGION16 updateRegion;
	RECTANGLE_16 updateRect;
	RECTANGLE_16* updateRects;
	REGION16 clippingRects;
	RECTANGLE_16 clippingRect;
	RFX_PROGRESSIVE_TILE* tile;
	PROGRESSIVE_BLOCK_REGION* region;

	freerdp_client_codecs_prepare(xfc->codecs, FREERDP_CODEC_PROGRESSIVE);

	surface = (xfGfxSurface*) context->GetSurfaceData(context, cmd->surfaceId);

	if (!surface)
		return -1;

	progressive_create_surface_context(xfc->codecs->progressive, cmd->surfaceId, surface->width, surface->height);

	DstData = surface->data;

	status = progressive_decompress(xfc->codecs->progressive, cmd->data, cmd->length, &DstData,
			surface->format, surface->scanline, cmd->left, cmd->top, cmd->width, cmd->height, cmd->surfaceId);

	if (status < 0)
	{
		WLog_ERR(TAG, "progressive_decompress failure: %d", status);
		return -1;
	}

	region = &(xfc->codecs->progressive->region);

	region16_init(&clippingRects);

	for (i = 0; i < region->numRects; i++)
	{
		rect = &(region->rects[i]);

		clippingRect.left = cmd->left + rect->x;
		clippingRect.top = cmd->top + rect->y;
		clippingRect.right = clippingRect.left + rect->width;
		clippingRect.bottom = clippingRect.top + rect->height;

		region16_union_rect(&clippingRects, &clippingRects, &clippingRect);
	}

	for (i = 0; i < region->numTiles; i++)
	{
		tile = region->tiles[i];

		updateRect.left = cmd->left + tile->x;
		updateRect.top = cmd->top + tile->y;
		updateRect.right = updateRect.left + 64;
		updateRect.bottom = updateRect.top + 64;

		region16_init(&updateRegion);
		region16_intersect_rect(&updateRegion, &clippingRects, &updateRect);
		updateRects = (RECTANGLE_16*) region16_rects(&updateRegion, &nbUpdateRects);

		for (j = 0; j < nbUpdateRects; j++)
		{
			nXDst = updateRects[j].left;
			nYDst = updateRects[j].top;
			nWidth = updateRects[j].right - updateRects[j].left;
			nHeight = updateRects[j].bottom - updateRects[j].top;

			nXSrc = nXDst - (cmd->left + tile->x);
			nYSrc = nYDst - (cmd->top + tile->y);

			freerdp_image_copy(surface->data, PIXEL_FORMAT_XRGB32,
					surface->scanline, nXDst, nYDst, nWidth, nHeight,
					tile->data, PIXEL_FORMAT_XRGB32, 64 * 4, nXSrc, nYSrc, NULL);

			region16_union_rect(&(xfc->invalidRegion), &(xfc->invalidRegion), &updateRects[j]);
		}

		region16_uninit(&updateRegion);
	}

	region16_uninit(&clippingRects);

	if (!xfc->inGfxFrame)
		xf_OutputUpdate(xfc);

	return 1;
}

int xf_SurfaceCommand(RdpgfxClientContext* context, RDPGFX_SURFACE_COMMAND* cmd)
{
	int status = 1;
	xfContext* xfc = (xfContext*) context->custom;

	switch (cmd->codecId)
	{
		case RDPGFX_CODECID_UNCOMPRESSED:
			status = xf_SurfaceCommand_Uncompressed(xfc, context, cmd);
			break;

		case RDPGFX_CODECID_CAVIDEO:
			status = xf_SurfaceCommand_RemoteFX(xfc, context, cmd);
			break;

		case RDPGFX_CODECID_CLEARCODEC:
			status = xf_SurfaceCommand_ClearCodec(xfc, context, cmd);
			break;

		case RDPGFX_CODECID_PLANAR:
			status = xf_SurfaceCommand_Planar(xfc, context, cmd);
			break;

		case RDPGFX_CODECID_H264:
			status = xf_SurfaceCommand_H264(xfc, context, cmd);
			break;

		case RDPGFX_CODECID_ALPHA:
			status = xf_SurfaceCommand_Alpha(xfc, context, cmd);
			break;

		case RDPGFX_CODECID_CAPROGRESSIVE:
			status = xf_SurfaceCommand_Progressive(xfc, context, cmd);
			break;

		case RDPGFX_CODECID_CAPROGRESSIVE_V2:
			break;
	}

	return 1;
}

int xf_DeleteEncodingContext(RdpgfxClientContext* context, RDPGFX_DELETE_ENCODING_CONTEXT_PDU* deleteEncodingContext)
{
	return 1;
}

int xf_CreateSurface(RdpgfxClientContext* context, RDPGFX_CREATE_SURFACE_PDU* createSurface)
{
	size_t size;
	UINT32 bytesPerPixel;
	xfGfxSurface* surface;
	xfContext* xfc = (xfContext*) context->custom;

	surface = (xfGfxSurface*) calloc(1, sizeof(xfGfxSurface));

	if (!surface)
		return -1;

	surface->surfaceId = createSurface->surfaceId;
	surface->width = (UINT32) createSurface->width;
	surface->height = (UINT32) createSurface->height;
	surface->alpha = (createSurface->pixelFormat == PIXEL_FORMAT_ARGB_8888) ? TRUE : FALSE;
	surface->format = PIXEL_FORMAT_XRGB32;

	surface->scanline = surface->width * 4;
	surface->scanline += (surface->scanline % (xfc->scanline_pad / 8));

	size = surface->scanline * surface->height;
	surface->data = (BYTE*) _aligned_malloc(size, 16);

	if (!surface->data)
	{
		free (surface);
		return -1;
	}

	ZeroMemory(surface->data, size);

	if ((xfc->depth == 24) || (xfc->depth == 32))
	{
		surface->image = XCreateImage(xfc->display, xfc->visual, xfc->depth, ZPixmap, 0,
				(char*) surface->data, surface->width, surface->height, xfc->scanline_pad, surface->scanline);
	}
	else
	{
		bytesPerPixel = (FREERDP_PIXEL_FORMAT_BPP(xfc->format) / 8);
		surface->stageStep = surface->width * bytesPerPixel;
		surface->stageStep += (surface->stageStep % (xfc->scanline_pad / 8));
		size = surface->stageStep * surface->height;

		surface->stage = (BYTE*) _aligned_malloc(size, 16);

		if (!surface->stage)
		{
			free (surface->data);
			free (surface);
			return -1;
		}

		ZeroMemory(surface->stage, size);

		surface->image = XCreateImage(xfc->display, xfc->visual, xfc->depth, ZPixmap, 0,
				(char*) surface->stage, surface->width, surface->height, xfc->scanline_pad, surface->stageStep);
	}

	context->SetSurfaceData(context, surface->surfaceId, (void*) surface);

	return 1;
}

int xf_DeleteSurface(RdpgfxClientContext* context, RDPGFX_DELETE_SURFACE_PDU* deleteSurface)
{
	xfGfxSurface* surface = NULL;
	xfContext* xfc = (xfContext*) context->custom;

	surface = (xfGfxSurface*) context->GetSurfaceData(context, deleteSurface->surfaceId);

	if (surface)
	{
		XFree(surface->image);
		_aligned_free(surface->data);
		_aligned_free(surface->stage);
		free(surface);
	}

	context->SetSurfaceData(context, deleteSurface->surfaceId, NULL);

	if (xfc->codecs->progressive)
		progressive_delete_surface_context(xfc->codecs->progressive, deleteSurface->surfaceId);

	return 1;
}

int xf_SolidFill(RdpgfxClientContext* context, RDPGFX_SOLID_FILL_PDU* solidFill)
{
	UINT16 index;
	UINT32 color;
	BYTE a, r, g, b;
	int nWidth, nHeight;
	RDPGFX_RECT16* rect;
	xfGfxSurface* surface;
	RECTANGLE_16 invalidRect;
	xfContext* xfc = (xfContext*) context->custom;

	surface = (xfGfxSurface*) context->GetSurfaceData(context, solidFill->surfaceId);

	if (!surface)
		return -1;

	b = solidFill->fillPixel.B;
	g = solidFill->fillPixel.G;
	r = solidFill->fillPixel.R;
	a = solidFill->fillPixel.XA;

	color = ARGB32(a, r, g, b);

	for (index = 0; index < solidFill->fillRectCount; index++)
	{
		rect = &(solidFill->fillRects[index]);

		nWidth = rect->right - rect->left;
		nHeight = rect->bottom - rect->top;

		invalidRect.left = rect->left;
		invalidRect.top = rect->top;
		invalidRect.right = rect->right;
		invalidRect.bottom = rect->bottom;

		freerdp_image_fill(surface->data, surface->format, surface->scanline,
				rect->left, rect->top, nWidth, nHeight, color);

		region16_union_rect(&(xfc->invalidRegion), &(xfc->invalidRegion), &invalidRect);
	}

	if (!xfc->inGfxFrame)
		xf_OutputUpdate(xfc);

	return 1;
}

int xf_SurfaceToSurface(RdpgfxClientContext* context, RDPGFX_SURFACE_TO_SURFACE_PDU* surfaceToSurface)
{
	UINT16 index;
	BOOL sameSurface;
	int nWidth, nHeight;
	RDPGFX_RECT16* rectSrc;
	RDPGFX_POINT16* destPt;
	RECTANGLE_16 invalidRect;
	xfGfxSurface* surfaceSrc;
	xfGfxSurface* surfaceDst;
	xfContext* xfc = (xfContext*) context->custom;

	rectSrc = &(surfaceToSurface->rectSrc);
	destPt = &surfaceToSurface->destPts[0];

	surfaceSrc = (xfGfxSurface*) context->GetSurfaceData(context, surfaceToSurface->surfaceIdSrc);

	sameSurface = (surfaceToSurface->surfaceIdSrc == surfaceToSurface->surfaceIdDest) ? TRUE : FALSE;

	if (!sameSurface)
		surfaceDst = (xfGfxSurface*) context->GetSurfaceData(context, surfaceToSurface->surfaceIdDest);
	else
		surfaceDst = surfaceSrc;

	if (!surfaceSrc || !surfaceDst)
		return -1;

	nWidth = rectSrc->right - rectSrc->left;
	nHeight = rectSrc->bottom - rectSrc->top;

	for (index = 0; index < surfaceToSurface->destPtsCount; index++)
	{
		destPt = &surfaceToSurface->destPts[index];

		if (sameSurface)
		{
			freerdp_image_move(surfaceDst->data, surfaceDst->format, surfaceDst->scanline,
					destPt->x, destPt->y, nWidth, nHeight, rectSrc->left, rectSrc->top);
		}
		else
		{
			freerdp_image_copy(surfaceDst->data, surfaceDst->format, surfaceDst->scanline,
					destPt->x, destPt->y, nWidth, nHeight, surfaceSrc->data, surfaceSrc->format,
					surfaceSrc->scanline, rectSrc->left, rectSrc->top, NULL);
		}

		invalidRect.left = destPt->x;
		invalidRect.top = destPt->y;
		invalidRect.right = destPt->x + rectSrc->right;
		invalidRect.bottom = destPt->y + rectSrc->bottom;

		region16_union_rect(&(xfc->invalidRegion), &(xfc->invalidRegion), &invalidRect);
	}

	if (!xfc->inGfxFrame)
		xf_OutputUpdate(xfc);

	return 1;
}

int xf_SurfaceToCache(RdpgfxClientContext* context, RDPGFX_SURFACE_TO_CACHE_PDU* surfaceToCache)
{
	size_t size;
	RDPGFX_RECT16* rect;
	xfGfxSurface* surface;
	xfGfxCacheEntry* cacheEntry;
	xfContext* xfc = (xfContext*) context->custom;

	rect = &(surfaceToCache->rectSrc);

	surface = (xfGfxSurface*) context->GetSurfaceData(context, surfaceToCache->surfaceId);

	if (!surface)
		return -1;

	cacheEntry = (xfGfxCacheEntry*) calloc(1, sizeof(xfGfxCacheEntry));

	if (!cacheEntry)
		return -1;

	cacheEntry->width = (UINT32) (rect->right - rect->left);
	cacheEntry->height = (UINT32) (rect->bottom - rect->top);
	cacheEntry->alpha = surface->alpha;
	cacheEntry->format = surface->format;

	cacheEntry->scanline = cacheEntry->width * 4;
	cacheEntry->scanline += (cacheEntry->scanline % (xfc->scanline_pad / 8));

	size = cacheEntry->scanline * cacheEntry->height;
	cacheEntry->data = (BYTE*) _aligned_malloc(size, 16);

	if (!cacheEntry->data)
	{
		free (cacheEntry);
		return -1;
	}

	ZeroMemory(cacheEntry->data, size);

	freerdp_image_copy(cacheEntry->data, cacheEntry->format, cacheEntry->scanline,
			0, 0, cacheEntry->width, cacheEntry->height, surface->data,
			surface->format, surface->scanline, rect->left, rect->top, NULL);

	context->SetCacheSlotData(context, surfaceToCache->cacheSlot, (void*) cacheEntry);

	return 1;
}

int xf_CacheToSurface(RdpgfxClientContext* context, RDPGFX_CACHE_TO_SURFACE_PDU* cacheToSurface)
{
	UINT16 index;
	RDPGFX_POINT16* destPt;
	xfGfxSurface* surface;
	xfGfxCacheEntry* cacheEntry;
	RECTANGLE_16 invalidRect;
	xfContext* xfc = (xfContext*) context->custom;

	surface = (xfGfxSurface*) context->GetSurfaceData(context, cacheToSurface->surfaceId);
	cacheEntry = (xfGfxCacheEntry*) context->GetCacheSlotData(context, cacheToSurface->cacheSlot);

	if (!surface || !cacheEntry)
		return -1;

	for (index = 0; index < cacheToSurface->destPtsCount; index++)
	{
		destPt = &cacheToSurface->destPts[index];

		freerdp_image_copy(surface->data, surface->format, surface->scanline,
				destPt->x, destPt->y, cacheEntry->width, cacheEntry->height,
				cacheEntry->data, cacheEntry->format, cacheEntry->scanline, 0, 0, NULL);

		invalidRect.left = destPt->x;
		invalidRect.top = destPt->y;
		invalidRect.right = destPt->x + cacheEntry->width - 1;
		invalidRect.bottom = destPt->y + cacheEntry->height - 1;

		region16_union_rect(&(xfc->invalidRegion), &(xfc->invalidRegion), &invalidRect);
	}

	if (!xfc->inGfxFrame)
		xf_OutputUpdate(xfc);

	return 1;
}

int xf_CacheImportReply(RdpgfxClientContext* context, RDPGFX_CACHE_IMPORT_REPLY_PDU* cacheImportReply)
{
	return 1;
}

int xf_EvictCacheEntry(RdpgfxClientContext* context, RDPGFX_EVICT_CACHE_ENTRY_PDU* evictCacheEntry)
{
	xfGfxCacheEntry* cacheEntry;

	cacheEntry = (xfGfxCacheEntry*) context->GetCacheSlotData(context, evictCacheEntry->cacheSlot);

	if (cacheEntry)
	{
		_aligned_free(cacheEntry->data);
		free(cacheEntry);
	}

	context->SetCacheSlotData(context, evictCacheEntry->cacheSlot, NULL);

	return 1;
}

int xf_MapSurfaceToOutput(RdpgfxClientContext* context, RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU* surfaceToOutput)
{
	xfContext* xfc = (xfContext*) context->custom;

	xfc->outputSurfaceId = surfaceToOutput->surfaceId;

	return 1;
}

int xf_MapSurfaceToWindow(RdpgfxClientContext* context, RDPGFX_MAP_SURFACE_TO_WINDOW_PDU* surfaceToWindow)
{
	return 1;
}

void xf_graphics_pipeline_init(xfContext* xfc, RdpgfxClientContext* gfx)
{
	xfc->gfx = gfx;
	gfx->custom = (void*) xfc;

	gfx->ResetGraphics = xf_ResetGraphics;
	gfx->StartFrame = xf_StartFrame;
	gfx->EndFrame = xf_EndFrame;
	gfx->SurfaceCommand = xf_SurfaceCommand;
	gfx->DeleteEncodingContext = xf_DeleteEncodingContext;
	gfx->CreateSurface = xf_CreateSurface;
	gfx->DeleteSurface = xf_DeleteSurface;
	gfx->SolidFill = xf_SolidFill;
	gfx->SurfaceToSurface = xf_SurfaceToSurface;
	gfx->SurfaceToCache = xf_SurfaceToCache;
	gfx->CacheToSurface = xf_CacheToSurface;
	gfx->CacheImportReply = xf_CacheImportReply;
	gfx->EvictCacheEntry = xf_EvictCacheEntry;
	gfx->MapSurfaceToOutput = xf_MapSurfaceToOutput;
	gfx->MapSurfaceToWindow = xf_MapSurfaceToWindow;

	region16_init(&(xfc->invalidRegion));
}

void xf_graphics_pipeline_uninit(xfContext* xfc, RdpgfxClientContext* gfx)
{
	region16_uninit(&(xfc->invalidRegion));

	gfx->custom = NULL;
	xfc->gfx = NULL;
}
