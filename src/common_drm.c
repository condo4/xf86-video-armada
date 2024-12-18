/*
 * Marvell Armada DRM-based driver
 *
 * Written by Russell King, 2012, derived in part from the
 * Intel xorg X server driver.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "xf86.h"
#include "cursorstr.h"

#include "boxutil.h"
#include "pixmaputil.h"

#include "backlight.h"
#include "common_drm.h"
#include "common_drm_conn.h"
#include "common_drm_helper.h"
#include "xf86_OSproc.h"
#include "xf86Crtc.h"
#include "xf86cmap.h"
#include "fb.h"
#include "micmap.h"
#include <xf86DDC.h>
#include <X11/extensions/dpmsconst.h>
#include <X11/Xatom.h>

#ifdef HAVE_UDEV
#include <libudev.h>
#endif

#if ABI_VIDEODRV_VERSION >= SET_ABI_VERSION(22,0)
#define HAVE_NOTIFY_FD	1
#endif

enum {
	OPTION_HW_CURSOR,
	OPTION_HOTPLUG,
	OPTION_PRESENT,
};

const OptionInfoRec common_drm_options[] = {
	{ OPTION_HW_CURSOR,	"HWcursor",	OPTV_BOOLEAN, {0}, FALSE },
	{ OPTION_HOTPLUG,	"HotPlug",	OPTV_BOOLEAN, {0}, TRUE  },
	{ OPTION_PRESENT,	"Present",	OPTV_BOOLEAN, {0}, FALSE },
	{ -1,			NULL,		OPTV_NONE,    {0}, FALSE }
};

struct common_drm_property {
	drmModePropertyPtr mode_prop;
	uint64_t value;
	int natoms;
	Atom *atoms;
};

struct common_conn_info {
	int drm_fd;
	int drm_id;
	int dpms_mode;
	struct backlight backlight;
	int backlight_active_level;
	int nprops;
	struct common_drm_property *props;
	drmModeConnectorPtr mode_output;
	drmModeEncoderPtr mode_encoder;
};

static DevPrivateKeyRec pixmap_key;

struct common_pixmap {
	uint32_t handle;
	void *data;
	xf86CrtcPtr crtc;
	uint64_t last_ust;
	uint64_t last_msc;
	int64_t delta_msc;
};

static struct common_pixmap *common_drm_pixmap(PixmapPtr pixmap)
{
	return dixGetPrivateAddr(&pixmap->devPrivates, &pixmap_key);
}

void common_drm_set_pixmap_data(PixmapPtr pixmap, uint32_t handle, void *data)
{
	struct common_pixmap *c = common_drm_pixmap(pixmap);

	c->handle = handle;
	c->data = data;
}

void *common_drm_get_pixmap_data(PixmapPtr pixmap)
{
	return common_drm_pixmap(pixmap)->data;
}

static void common_drm_conn_backlight_set(xf86OutputPtr output, int level)
{
	struct common_conn_info *conn = output->driver_private;
	if (backlight_set(&conn->backlight, level) < 0) {
		xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
			   "failed to set backlight %s to brightness level %d, "
			   "disabling\n",
			   conn->backlight.iface, level);
		backlight_disable(&conn->backlight);
	}
}


static int common_drm_conn_backlight_get(xf86OutputPtr output)
{
	struct common_conn_info *conn = output->driver_private;
	return backlight_get(&conn->backlight);
}

static void common_drm_conn_backlight_init(xf86OutputPtr output)
{
	struct common_conn_info *conn = output->driver_private;

	/* Only add the backlight to LVDS */
	if (strcmp(output->name, "LVDS1"))
		return;

	conn->backlight_active_level =
		backlight_open(&conn->backlight, NULL);
	if (conn->backlight_active_level != -1) {
		xf86DrvMsg(output->scrn->scrnIndex, X_PROBED,
			   "found backlight control interface %s\n",
			   conn->backlight.iface);
		return;
	}
}

static void drmmode_ConvertFromKMode(ScrnInfoPtr pScrn,
	drmModeModeInfoPtr kmode, DisplayModePtr mode)
{
	memset(mode, 0, sizeof(*mode));

	mode->status = MODE_OK;
	mode->Clock = kmode->clock;
	mode->HDisplay = kmode->hdisplay;
	mode->HSyncStart = kmode->hsync_start;
	mode->HSyncEnd = kmode->hsync_end;
	mode->HTotal = kmode->htotal;
	mode->HSkew = kmode->hskew;
	mode->VDisplay = kmode->vdisplay;
	mode->VSyncStart = kmode->vsync_start;
	mode->VSyncEnd = kmode->vsync_end;
	mode->VTotal = kmode->vtotal;
	mode->VScan = kmode->vscan;
	mode->Flags = kmode->flags;
	mode->name = strdup(kmode->name);
	if (kmode->type & DRM_MODE_TYPE_DRIVER)
		mode->type = M_T_DRIVER;
	if (kmode->type & DRM_MODE_TYPE_PREFERRED)
		mode->type |= M_T_PREFERRED;
	xf86SetModeCrtc (mode, pScrn->adjustFlags);
}

static uint64_t common_drm_frame_to_msc(xf86CrtcPtr crtc, uint32_t seq)
{
	struct common_crtc_info *drmc = common_crtc(crtc);

	/*
	 * The sequence counter wrapped.  Unlike the misleading comments in
	 * xf86-video-intel, the vblank counter is never wound backwards: it
	 * always runs forwards.  However, since we don't monitor it with
	 * any regularity, it can appear to go backwards if we wait (eg)
	 * 0xc0000000 vblanks between calls.  Hence, whenever we see the
	 * frame sequence less than the last sequence, assume that it has
	 * wrapped.  (It may have wrapped more than once.)
	 */
	if (seq < drmc->last_seq)
		drmc->last_msc += 0x100000000ULL;

	drmc->last_seq = seq;

	return drmc->last_msc + seq;
}

static uint32_t common_drm_msc_to_frame(xf86CrtcPtr crtc, uint64_t msc)
{
	struct common_crtc_info *drmc = common_crtc(crtc);

	return msc - drmc->last_msc;
}

static drmModePropertyPtr common_drm_conn_find_property(
	struct common_conn_info *conn, const char *name, uint32_t *blob)
{
	drmModeConnectorPtr koutput = conn->mode_output;
	int i;

	for (i = 0; i < koutput->count_props; i++) {
		drmModePropertyPtr p;

		p = drmModeGetProperty(conn->drm_fd, koutput->props[i]);
		if (!p || (blob && !(p->flags & DRM_MODE_PROP_BLOB)))
			continue;

		if (!strcmp(p->name, name)) {
			if (blob)
				*blob = koutput->prop_values[i];
			return p;
		}

		drmModeFreeProperty(p);
	}
	return NULL;
}

static void
common_drm_conn_create_ranged_atom(xf86OutputPtr output, Atom *atom,
				   const char *name, INT32 min, INT32 max,
				   uint64_t value, Bool immutable)
{
	int err;
	INT32 atom_range[2];

	atom_range[0] = min;
	atom_range[1] = max;

	*atom = MakeAtom(name, strlen(name), TRUE);

	err = RRConfigureOutputProperty(output->randr_output, *atom, FALSE,
					TRUE, immutable, 2, atom_range);
	if (err != 0)
		xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
			   "RRConfigureOutputProperty error, %d\n", err);

	err = RRChangeOutputProperty(output->randr_output, *atom, XA_INTEGER,
				     32, PropModeReplace, 1, &value, FALSE,
				     FALSE);
	if (err != 0)
		xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
			   "RRChangeOutputProperty error, %d\n", err);
}

#define BACKLIGHT_NAME			"Backlight"
#define BACKLIGHT_DEPRECATED_NAME	"BACKLIGHT"
static Atom backlight_atom, backlight_deprecated_atom;

static void common_drm_conn_create_resources(xf86OutputPtr output)
{
	struct common_conn_info *conn = output->driver_private;
	drmModeConnectorPtr mop = conn->mode_output;
	int i, j, n, err;

	conn->props = calloc(mop->count_props, sizeof *conn->props);
	if (!conn->props)
		return;

	common_drm_conn_backlight_init(output);

	for (i = n = 0; i < mop->count_props; i++) {
		struct common_drm_property *prop = &conn->props[n];
		drmModePropertyPtr dprop;
		Bool immutable;

		dprop = drmModeGetProperty(conn->drm_fd, mop->props[i]);
		if (!dprop || dprop->flags & DRM_MODE_PROP_BLOB ||
		    !strcmp(dprop->name, "DPMS") ||
		    !strcmp(dprop->name, "EDID")) {
			drmModeFreeProperty(dprop);
			continue;
		}

		n++;
		prop->mode_prop = dprop;
		prop->value = mop->prop_values[i];

		immutable = dprop->flags & DRM_MODE_PROP_IMMUTABLE ?
				TRUE : FALSE;

		if (dprop->flags & DRM_MODE_PROP_RANGE) {
			INT32 range[2];
			uint32_t value = prop->value;

			prop->natoms = 1;
			prop->atoms = calloc(prop->natoms, sizeof *prop->atoms);
			if (!prop->atoms)
				continue;

			range[0] = dprop->values[0];
			range[1] = dprop->values[1];

			prop->atoms[0] = MakeAtom(dprop->name,
						  strlen(dprop->name), TRUE);
			err = RRConfigureOutputProperty(output->randr_output,
							prop->atoms[0], FALSE,
							TRUE, immutable, 2,
							range);
			if (err)
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
					   "RRConfigureOutputProperty error %d\n",
					   err);

			err = RRChangeOutputProperty(output->randr_output,
						     prop->atoms[0],
						     XA_INTEGER, 32,
					             PropModeReplace, 1,
						     &value, FALSE, TRUE);
			if (err)
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
					   "RRChangeOutputProperty error %d\n",
					   err);
		} else if (dprop->flags & DRM_MODE_PROP_ENUM) {
			int current;

			prop->natoms = dprop->count_enums + 1;
			prop->atoms = calloc(prop->natoms, sizeof *prop->atoms);
			if (!prop->atoms)
				continue;

			current = prop->natoms;
			prop->atoms[0] = MakeAtom(dprop->name,
						  strlen(dprop->name), TRUE);
			for (j = 1; j < prop->natoms; j++) {
				struct drm_mode_property_enum *e;

				e = &dprop->enums[j - 1];
				prop->atoms[j] = MakeAtom(e->name,
							  strlen(e->name),
							  TRUE);
				if (prop->value == e->value)
					current = j;
			}

			err = RRConfigureOutputProperty(output->randr_output,
						 prop->atoms[0], FALSE, FALSE,
						 immutable, prop->natoms - 1,
						 (INT32 *)&prop->atoms[1]);
			if (err)
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
					   "RRConfigureOutputProperty error, %d\n",
					   err);

			err = RRChangeOutputProperty(output->randr_output,
						     prop->atoms[0], XA_ATOM,
						     32, PropModeReplace, 1,
						     &prop->atoms[current],
						     FALSE, TRUE);
			if (err)
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
					   "RRChangeOutputProperty error, %d\n",
					   err);
		}
	}
	conn->nprops = n;

	if (conn->backlight.iface) {
		/* Set up the backlight property, which takes effect
		 * immediately and accepts values only within the
		 * backlight_range.
		 */
		common_drm_conn_create_ranged_atom(output, &backlight_atom,
					BACKLIGHT_NAME, 0,
					conn->backlight.max,
					conn->backlight_active_level,
					FALSE);
		common_drm_conn_create_ranged_atom(output,
					&backlight_deprecated_atom,
					BACKLIGHT_DEPRECATED_NAME, 0,
					conn->backlight.max,
					conn->backlight_active_level,
					FALSE);
	}
}

static void
common_drm_conn_dpms_backlight(xf86OutputPtr output, int oldmode, int mode)
{
	struct common_conn_info *conn = output->driver_private;

	if (!conn->backlight.iface)
		return;

	if (mode == DPMSModeOn) {
		/* If we're going from off->on we may need to turn on the backlight. */
		if (oldmode != DPMSModeOn)
			common_drm_conn_backlight_set(output,
					   conn->backlight_active_level);
	} else {
		/* Only save the current backlight value if we're going from on to off. */
		if (oldmode == DPMSModeOn)
			conn->backlight_active_level = common_drm_conn_backlight_get(output);
		common_drm_conn_backlight_set(output, 0);
	}
}

static void common_drm_conn_dpms(xf86OutputPtr output, int mode)
{
	struct common_conn_info *conn = output->driver_private;
	drmModePropertyPtr p = common_drm_conn_find_property(conn, "DPMS", NULL);

	if (p) {
		/* Make sure to reverse the order between on and off. */
		if (mode != DPMSModeOn)
			common_drm_conn_dpms_backlight(output,
						       conn->dpms_mode,
						       mode);

		drmModeConnectorSetProperty(conn->drm_fd, conn->drm_id,
					    p->prop_id, mode);

		if (mode == DPMSModeOn)
			common_drm_conn_dpms_backlight(output,
						       conn->dpms_mode,
						       mode);

		conn->dpms_mode = mode;
		drmModeFreeProperty(p);
	}
}

static xf86OutputStatus common_drm_conn_detect(xf86OutputPtr output)
{
	struct common_conn_info *conn = output->driver_private;
	xf86OutputStatus status = XF86OutputStatusUnknown;
	drmModeConnectorPtr koutput;

	koutput = drmModeGetConnector(conn->drm_fd, conn->drm_id);
	if (!koutput)
		return XF86OutputStatusUnknown;

	drmModeFreeConnector(conn->mode_output);
	conn->mode_output = koutput;

	switch (koutput->connection) {
	case DRM_MODE_CONNECTED:
		status = XF86OutputStatusConnected;
		break;
	case DRM_MODE_DISCONNECTED:
		status = XF86OutputStatusDisconnected;
		break;
	case DRM_MODE_UNKNOWNCONNECTION:
		break;
	}
	return status;
}

static Bool
common_drm_conn_mode_valid(xf86OutputPtr output, DisplayModePtr pModes)
{
	return MODE_OK;
}

static DisplayModePtr common_drm_conn_get_modes(xf86OutputPtr output)
{
	ScrnInfoPtr pScrn = output->scrn;
	struct common_conn_info *conn = output->driver_private;
	drmModePropertyBlobPtr edid = NULL;
	DisplayModePtr modes = NULL;
	drmModePropertyPtr p;
	xf86MonPtr mon;
	uint32_t blob;
	int i;

	p = common_drm_conn_find_property(conn, "EDID", &blob);
	if (p) {
		edid = drmModeGetPropertyBlob(conn->drm_fd, blob);
		drmModeFreeProperty(p);
	}

	mon = xf86InterpretEDID(pScrn->scrnIndex, edid ? edid->data : NULL);
	if (mon && edid->length > 128)
		mon->flags |= MONITOR_EDID_COMPLETE_RAWDATA;
	xf86OutputSetEDID(output, mon);

	/* modes should already be available */
	for (i = 0; i < conn->mode_output->count_modes; i++) {
		DisplayModePtr mode = xnfalloc(sizeof *mode);

		drmmode_ConvertFromKMode(pScrn, &conn->mode_output->modes[i], mode);
		modes = xf86ModesAdd(modes, mode);
	}

	return modes;
}

#ifdef RANDR_12_INTERFACE
static Bool common_drm_conn_set_property(xf86OutputPtr output, Atom property,
	RRPropertyValuePtr value)
{
	struct common_conn_info *conn = output->driver_private;
	int i;

	if (property == backlight_atom || property == backlight_deprecated_atom) {
		INT32 val;

		if (value->type != XA_INTEGER || value->format != 32 ||
		    value->size != 1)
		{
			return FALSE;
		}

		val = *(INT32 *)value->data;
		if (val < 0 || val > conn->backlight.max)
			return FALSE;

		if (conn->dpms_mode == DPMSModeOn)
			common_drm_conn_backlight_set(output, val);
		conn->backlight_active_level = val;
		return TRUE;
	}

	for (i = 0; i < conn->nprops; i++) {
		struct common_drm_property *prop = &conn->props[i];
		drmModePropertyPtr dprop;

		if (prop->atoms[0] != property)
			continue;

		dprop = prop->mode_prop;
		if (dprop->flags & DRM_MODE_PROP_RANGE) {
			if (value->type != XA_INTEGER || value->format != 32 || value->size != 1)
				return FALSE;

			drmModeConnectorSetProperty(conn->drm_fd, conn->drm_id,
					dprop->prop_id, (uint64_t)*(uint32_t *)value->data);

			return TRUE;
		} else if (dprop->flags & DRM_MODE_PROP_ENUM) {
			Atom atom;
			const char *name;
			int j;

			if (value->type != XA_ATOM || value->format != 32 || value->size != 1)
				return FALSE;

			memcpy(&atom, value->data, sizeof(atom));
			name = NameForAtom(atom);
			if (name == NULL)
				return FALSE;

			for (j = 0; j < dprop->count_enums; j++) {
				if (!strcmp(dprop->enums[j].name, name)) {
					drmModeConnectorSetProperty(conn->drm_fd,
						conn->drm_id, dprop->prop_id,
						dprop->enums[j].value);
					return TRUE;
				}
			}
			return FALSE;
		}
	}
	return TRUE;
}
#endif
#ifdef RANDR_13_INTERFACE
static Bool common_drm_conn_get_property(xf86OutputPtr output, Atom property)
{
	struct common_conn_info *conn = output->driver_private;
	int err;

	if (property == backlight_atom || property == backlight_deprecated_atom) {
		INT32 val;

		if (!conn->backlight.iface)
			return FALSE;

		if (conn->dpms_mode == DPMSModeOn) {
			val = common_drm_conn_backlight_get(output);
			if (val < 0)
				return FALSE;
		} else {
			val = conn->backlight_active_level;
		}

		err = RRChangeOutputProperty(output->randr_output, property,
					     XA_INTEGER, 32, PropModeReplace, 1, &val,
					     FALSE, FALSE);
		if (err != 0) {
			xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
				   "RRChangeOutputProperty error, %d\n", err);
			return FALSE;
		}

		return TRUE;
	}

	return FALSE;
}
#endif

static void common_drm_conn_destroy(xf86OutputPtr output)
{
	struct common_conn_info *conn = output->driver_private;

	drmModeFreeConnector(conn->mode_output);
	drmModeFreeEncoder(conn->mode_encoder);
	free(conn);

	output->driver_private = NULL;
}

static const xf86OutputFuncsRec drm_output_funcs = {
	.create_resources = common_drm_conn_create_resources,
	.dpms = common_drm_conn_dpms,
	.detect = common_drm_conn_detect,
	.mode_valid = common_drm_conn_mode_valid,
	.get_modes = common_drm_conn_get_modes,
#ifdef RANDR_12_INTERFACE
	.set_property = common_drm_conn_set_property,
#endif
#ifdef RANDR_13_INTERFACE
	.get_property = common_drm_conn_get_property,
#endif
	.destroy = common_drm_conn_destroy,
};

static const char *const output_names[] = {
	"None", "VGA", "DVI", "DVI", "DVI", "Composite", "TV",
	"LVDS", "CTV", "DIN", "DP", "HDMI", "HDMI", "TV", "eDP"
};

static const int subpixel_conv_table[] = {
	0, SubPixelUnknown, SubPixelHorizontalRGB, SubPixelHorizontalBGR,
	SubPixelVerticalRGB, SubPixelVerticalBGR, SubPixelNone
};

static void common_drm_conn_init(ScrnInfoPtr pScrn, uint32_t id)
{
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	drmModeConnectorPtr koutput;
	drmModeEncoderPtr kencoder;
	xf86OutputPtr output;
	struct common_conn_info *conn;
	char name[32];

	koutput = drmModeGetConnector(drm->fd, id);
	if (!koutput)
		return;

	kencoder = drmModeGetEncoder(drm->fd, koutput->encoders[0]);
	if (!kencoder) {
		drmModeFreeConnector(koutput);
		return;
	}

	snprintf(name, sizeof(name), "%s%d",
		 output_names[koutput->connector_type],
		 koutput->connector_type_id);

	output = xf86OutputCreate(pScrn, &drm_output_funcs, name);
	if (!output) {
		drmModeFreeEncoder(kencoder);
		drmModeFreeConnector(koutput);
		return;
	}

	conn = calloc(1, sizeof *conn);
	if (!conn) {
		xf86OutputDestroy(output);
		drmModeFreeEncoder(kencoder);
		drmModeFreeConnector(koutput);
		return;
	}

	conn->drm_fd = drm->fd;
	conn->drm_id = id;
	conn->mode_output = koutput;
	conn->mode_encoder = kencoder;

	output->driver_private = conn;
	output->mm_width = koutput->mmWidth;
	output->mm_height = koutput->mmHeight;
	output->subpixel_order = subpixel_conv_table[koutput->subpixel];
	output->possible_crtcs = kencoder->possible_crtcs;
	output->possible_clones = kencoder->possible_clones;
	output->interlaceAllowed = 1; /* wish there was a way to read that */
	output->doubleScanAllowed = 0;
}

/*
 * CRTC support
 */
static void common_drm_reload_hw_cursors(ScrnInfoPtr pScrn)
{
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);

	/* Work around stricter checks in X */
	if (pScrn->pScreen && drm->hw_cursor)
		xf86_reload_cursors(pScrn->pScreen);
}

static void drmmode_ConvertToKMode(drmModeModeInfoPtr kmode,
	DisplayModePtr mode)
{
	memset(kmode, 0, sizeof(*kmode));

	kmode->clock = mode->Clock;
	kmode->hdisplay = mode->HDisplay;
	kmode->hsync_start = mode->HSyncStart;
	kmode->hsync_end = mode->HSyncEnd;
	kmode->htotal = mode->HTotal;
	kmode->hskew = mode->HSkew;
	kmode->vdisplay = mode->VDisplay;
	kmode->vsync_start = mode->VSyncStart;
	kmode->vsync_end = mode->VSyncEnd;
	kmode->vtotal = mode->VTotal;
	kmode->vscan = mode->VScan;
	kmode->flags = mode->Flags;
	if (mode->name)
		strncpy(kmode->name, mode->name, DRM_DISPLAY_MODE_LEN);
	kmode->name[DRM_DISPLAY_MODE_LEN-1] = 0;
}

static Bool common_drm_crtc_apply(xf86CrtcPtr crtc, uint32_t front_fb_id)
{
	ScrnInfoPtr pScrn = crtc->scrn;
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	struct common_crtc_info *drmc = common_crtc(crtc);
	drmModeModeInfo kmode;
	uint32_t fb_id, *output_ids;
	int x, y, i, ret, output_num;

	output_ids = calloc(xf86_config->num_output, sizeof *output_ids);
	if (!output_ids)
		return FALSE;

	output_num = common_drm_conn_output_ids(crtc, output_ids);

	if (!xf86CrtcRotate(crtc)) {
		ret = FALSE;
		goto done;
	}

	crtc->funcs->gamma_set(crtc, crtc->gamma_red, crtc->gamma_green,
			       crtc->gamma_blue, crtc->gamma_size);

	if (drmc->rotate_fb_id) {
		fb_id = drmc->rotate_fb_id;
		x = y = 0;
	} else {
		fb_id = front_fb_id;
		x = crtc->x;
		y = crtc->y;
	}

	drmmode_ConvertToKMode(&kmode, &crtc->mode);

	ret = drmModeSetCrtc(drmc->drm_fd, drmc->drm_id, fb_id,
			     x, y, output_ids, output_num, &kmode);
	if (ret) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to set mode on crtc %u: %s\n",
			   drmc->drm_id, strerror(errno));
		ret = FALSE;
	} else {
		ret = TRUE;

		for (i = 0; i < xf86_config->num_output; i++) {
			xf86OutputPtr output = xf86_config->output[i];

			if (output->crtc != crtc)
				continue;

			output->funcs->dpms(output, DPMSModeOn);
		}
	}

done:
	free(output_ids);
	return ret;
}

void common_drm_crtc_dpms(xf86CrtcPtr crtc, int mode)
{
}

Bool common_drm_crtc_set_mode_major(xf86CrtcPtr crtc, DisplayModePtr mode,
	Rotation rotation, int x, int y)
{
	struct common_drm_info *drm = GET_DRM_INFO(crtc->scrn);
	DisplayModeRec saved_mode;
	Rotation saved_rotation;
	int ret, saved_x, saved_y;

	saved_mode = crtc->mode;
	saved_x = crtc->x;
	saved_y = crtc->y;
	saved_rotation = crtc->rotation;
	crtc->mode = *mode;
	crtc->x = x;
	crtc->y = y;
	crtc->rotation = rotation;

	ret = common_drm_crtc_apply(crtc, drm->fb_id);
	if (!ret) {
		crtc->mode = saved_mode;
		crtc->x = saved_x;
		crtc->y = saved_y;
		crtc->rotation = saved_rotation;
	}

	common_drm_reload_hw_cursors(crtc->scrn);

	return ret;
}

void common_drm_crtc_resize(ScrnInfoPtr pScrn, int width, int height,
	int displayWidth, uint32_t fb_id)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	uint32_t old_fb_id;
	int i;

	pScrn->virtualX = width;
	pScrn->virtualY = height;
	pScrn->displayWidth = displayWidth;

	old_fb_id = drm->fb_id;
	drm->fb_id = fb_id;

	for (i = 0; i < xf86_config->num_crtc; i++) {
		xf86CrtcPtr crtc = xf86_config->crtc[i];

		if (!crtc->enabled)
			continue;

		common_drm_crtc_apply(crtc, fb_id);
	}

	common_drm_reload_hw_cursors(pScrn);

	drmModeRmFB(drm->fd, old_fb_id);
}

void common_drm_crtc_gamma_set(xf86CrtcPtr crtc,
	CARD16 *red, CARD16 *green, CARD16 *blue, int size)
{
	struct common_crtc_info *drmc = common_crtc(crtc);

	drmModeCrtcSetGamma(drmc->drm_fd, drmc->drm_id,
			    size, red, green, blue);
}

void common_drm_crtc_set_cursor_position(xf86CrtcPtr crtc, int x, int y)
{
	struct common_crtc_info *drmc = common_crtc(crtc);

	drmModeMoveCursor(drmc->drm_fd, drmc->drm_id, x, y);
}

void common_drm_crtc_show_cursor(xf86CrtcPtr crtc)
{
	struct common_drm_info *drm = GET_DRM_INFO(crtc->scrn);
	struct common_crtc_info *drmc = common_crtc(crtc);
	uint32_t crtc_id = drmc->drm_id;
	uint32_t handle = drmc->cursor_handle;
	uint32_t width = drm->cursor_max_width;
	uint32_t height = drm->cursor_max_height;

	if (drmc->has_cursor2) {
		xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(crtc->scrn);
		CursorBitsPtr cursor_bits = config->cursor->bits;

		drmModeSetCursor2(drmc->drm_fd, crtc_id, handle, width, height,
				  cursor_bits->xhot, cursor_bits->yhot);
	} else {
		drmModeSetCursor(drmc->drm_fd, crtc_id, handle, width, height);
	}
}

void common_drm_crtc_hide_cursor(xf86CrtcPtr crtc)
{
	struct common_crtc_info *drmc = common_crtc(crtc);

	drmModeSetCursor(drmc->drm_fd, drmc->drm_id, 0, 0, 0);
}

Bool common_drm_crtc_shadow_allocate(xf86CrtcPtr crtc, int width, int height,
	uint32_t pitch, uint32_t handle)
{
	struct common_crtc_info *drmc = common_crtc(crtc);
	ScrnInfoPtr pScrn = crtc->scrn;
	int ret;

	ret = drmModeAddFB(drmc->drm_fd, width, height, pScrn->depth,
			   pScrn->bitsPerPixel, pitch, handle,
			   &drmc->rotate_fb_id);
	if (ret) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Failed to add rotate fb: %s\n",
			   strerror(errno));
		return FALSE;
	} else {
		GET_DRM_INFO(crtc->scrn)->shadow_present++;
	}

	return TRUE;
}

void common_drm_crtc_shadow_destroy(xf86CrtcPtr crtc)
{
	struct common_crtc_info *drmc = common_crtc(crtc);

	drmModeRmFB(drmc->drm_fd, drmc->rotate_fb_id);
	drmc->rotate_fb_id = 0;
	GET_DRM_INFO(crtc->scrn)->shadow_present--;
}

static Bool common_drm_crtc_init(ScrnInfoPtr pScrn, unsigned num,
	const xf86CrtcFuncsRec *funcs)
{
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	struct common_crtc_info *drmc;
	xf86CrtcPtr crtc;
	uint32_t id;

	id = drm->mode_res->crtcs[num];

	crtc = xf86CrtcCreate(pScrn, funcs);
	if (!crtc)
		return FALSE;

	drmc = xnfcalloc(1, sizeof *drmc);
	drmc->drm_fd = drm->fd;
	drmc->drm_id = id;
	drmc->num = num;
	crtc->driver_private = drmc;

	/* Test whether hardware cursor is supported */
	if (drmModeSetCursor(drmc->drm_fd, id, 0, 0, 0))
		drm->has_hw_cursor = FALSE;
	else if (!drmModeSetCursor2(drmc->drm_fd, id, 0, 0, 0, 0, 0))
		drmc->has_cursor2 = TRUE;

	return TRUE;
}

static void common_drm_event(int fd, unsigned int frame, unsigned int tv_sec,
	unsigned int tv_usec, void *event_data)
{
	struct common_drm_event *event = event_data;
	struct common_crtc_info *drmc = common_crtc(event->crtc);
	uint64_t msc = common_drm_frame_to_msc(event->crtc, frame);

	drmc->swap_msc = msc;
	drmc->swap_ust = ((CARD64)tv_sec * 1000000) + tv_usec;

	event->handler(event, msc, tv_sec, tv_usec);
}

Bool common_drm_init_mode_resources(ScrnInfoPtr pScrn,
	const xf86CrtcFuncsRec *funcs)
{
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	Gamma zeros = { 0.0, 0.0, 0.0 };
	int i;

	drm->event_context.version = DRM_EVENT_CONTEXT_VERSION;
	drm->event_context.vblank_handler = common_drm_event;
	drm->event_context.page_flip_handler = common_drm_event;

	drm->mode_res = drmModeGetResources(drm->fd);
	if (!drm->mode_res) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "failed to get resources: %s\n", strerror(errno));
		return FALSE;
	}

	xf86CrtcSetSizeRange(pScrn, drm->mode_res->min_width,
			     drm->mode_res->min_height,
			     drm->mode_res->max_width,
			     drm->mode_res->max_height);

	drm->has_hw_cursor = TRUE;
	for (i = 0; i < drm->mode_res->count_crtcs; i++)
		if (!common_drm_crtc_init(pScrn, i, funcs))
			return FALSE;

	for (i = 0; i < drm->mode_res->count_connectors; i++)
		common_drm_conn_init(pScrn, drm->mode_res->connectors[i]);

	xf86InitialConfiguration(pScrn, TRUE);

	if (!xf86SetGamma(pScrn, zeros))
		return FALSE;

	if (pScrn->modes == NULL) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "No modes.\n");
		return FALSE;
	}

	pScrn->currentMode = pScrn->modes;

	/* Set display resolution */
	xf86SetDpi(pScrn, 0, 0);

	if (!xf86LoadSubModule(pScrn, "fb"))
		return FALSE;

	return TRUE;
}

static void common_drm_flip_handler(struct common_drm_event *event,
	uint64_t msc, unsigned int tv_sec, unsigned int tv_usec)
{
	struct common_drm_info *drm = event->drm;

	if (drm->flip_ref_crtc == event->crtc) {
		drm->flip_msc = msc;
		drm->flip_tv_sec = tv_sec;
		drm->flip_tv_usec = tv_usec;
	}

	free(event);

	if (--drm->flip_count)
		return;

	drmModeRmFB(drm->fd, drm->flip_old_fb_id);

	/* Now pass the event on to the flip complete event handler */
	event = drm->flip_event;
	if (event)
		event->handler(event, drm->flip_msc, drm->flip_tv_sec,
			       drm->flip_tv_usec);
}

_X_EXPORT
Bool common_drm_flip(ScrnInfoPtr pScrn, PixmapPtr pixmap,
	struct common_drm_event *event, xf86CrtcPtr ref_crtc)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	uint32_t old_fb_id, pitch, handle;
	int i;

	old_fb_id = drm->fb_id;

	pitch = pixmap->devKind;
	handle = common_drm_pixmap(pixmap)->handle;

	if (drmModeAddFB(drm->fd, pScrn->virtualX, pScrn->virtualY,
			 pScrn->depth, pScrn->bitsPerPixel, pitch, handle,
			 &drm->fb_id)) {
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "page flip: add fb failed: %s\n", strerror(errno));
		goto err;
	}

	for (i = 0; i < xf86_config->num_crtc; i++) {
		xf86CrtcPtr crtc = xf86_config->crtc[i];
		struct common_crtc_info *drmc;
		struct common_drm_event *event;

		if (!crtc->enabled)
			continue;

		event = calloc(1, sizeof *event);
		if (!event) {
			xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
				   "page flip: malloc failed\n");
			continue;
		}

		event->crtc = crtc;
		event->drm = drm;
		event->handler = common_drm_flip_handler;

		drmc = common_crtc(crtc);
		if (drmModePageFlip(drm->fd, drmc->drm_id, drm->fb_id,
				    DRM_MODE_PAGE_FLIP_EVENT, event)) {
			xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
				   "page flip: queue failed: %s\n", strerror(errno));
			free(event);
			continue;
		}
		drm->flip_count++;
	}

	if (drm->flip_count) {
		drm->flip_event = event;
		drm->flip_ref_crtc = ref_crtc;
		drm->flip_msc = 0;
		drm->flip_tv_sec = 0;
		drm->flip_tv_usec = 0;
		drm->flip_old_fb_id = old_fb_id;
		return TRUE;
	}

	drmModeRmFB(drm->fd, drm->fb_id);
	drm->fb_id = old_fb_id;
 err:
	return FALSE;
}

void common_drm_flip_pixmap(ScreenPtr pScreen, PixmapPtr front, PixmapPtr b)
{
	struct common_pixmap front_c;
	RegionRec region;
	void *front_ptr;

	/* Swap the pointers */
	front_ptr = front->devPrivate.ptr;
	front->devPrivate.ptr = b->devPrivate.ptr;
	b->devPrivate.ptr = front_ptr;

	/* Swap the common pixmap data (bo pointer and handle) */
	front_c = *common_drm_pixmap(front);
	*common_drm_pixmap(front) = *common_drm_pixmap(b);
	*common_drm_pixmap(b) = front_c;

	/* Mark the front pixmap as having changed */
	box_init(&region.extents, 0, 0,
		 front->drawable.width, front->drawable.height);
	region.data = NULL;

	DamageRegionAppend(&front->drawable, &region);
	DamageRegionProcessPending(&front->drawable);
}

void common_drm_LoadPalette(ScrnInfoPtr pScrn, int num, int *indices,
	LOCO *colors, VisualPtr pVisual)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	uint16_t lut_r[256], lut_g[256], lut_b[256];
	int i, p;

	for (i = 0; i < num; i++) {
		int index = indices[i];
		lut_r[index] = colors[index].red << 8;
		lut_g[index] = colors[index].green << 8;
		lut_b[index] = colors[index].blue << 8;
	}

	for (p = 0; p < xf86_config->num_crtc; p++) {
		xf86CrtcPtr crtc = xf86_config->crtc[p];

#ifdef RANDR_12_INTERFACE
		RRCrtcGammaSet(crtc->randr_crtc, lut_r, lut_g, lut_b);
#else
		crtc->funcs->gamma_set(crtc, lut_r, lut_g, lut_b, 256);
#endif
	}
}

#ifdef HAVE_UDEV
static void common_drm_handle_uevent(int fd, pointer data)
{
	ScrnInfoPtr pScrn = data;
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	struct udev_device *ud;

	ud = udev_monitor_receive_device(drm->udev.monitor);
	if (ud) {
		dev_t dev = udev_device_get_devnum(ud);
		const char *hp = udev_device_get_property_value(ud, "HOTPLUG");

		if (dev == drm->udev.drm_dev && hp && strtol(hp, NULL, 10) == 1)
			RRGetInfo(screenInfo.screens[pScrn->scrnIndex], TRUE);

		udev_device_unref(ud);
	}
}

static Bool common_drm_udev_CloseScreen(CLOSE_SCREEN_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);

	if (drm->udev.monitor) {
		struct udev *udev = udev_monitor_get_udev(drm->udev.monitor);

		xf86RemoveGeneralHandler(drm->udev.handler);
		udev_monitor_unref(drm->udev.monitor);
		udev_unref(udev);
	}

	pScreen->CloseScreen = drm->udev.CloseScreen;

	return pScreen->CloseScreen(CLOSE_SCREEN_ARGS);
}

static Bool common_drm_udev_init(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	MessageType from = X_CONFIG;
	struct udev_monitor *udev_mon;
	struct udev *udev;
	struct stat st;
	Bool hotplug;

	if (!xf86GetOptValBool(drm->Options, OPTION_HOTPLUG, &hotplug)) {
		from = X_DEFAULT;
		hotplug = TRUE;
	}

	xf86DrvMsg(pScrn->scrnIndex, from, "hotplug detection %sabled\n",
		   hotplug ? "en" : "dis");
	if (!hotplug)
		return TRUE;

	if (fstat(drm->fd, &st) || !S_ISCHR(st.st_mode))
		return FALSE;

	drm->udev.drm_dev = st.st_rdev;

	udev = udev_new();
	if (!udev)
		return FALSE;

	udev_mon = udev_monitor_new_from_netlink(udev, "udev");
	if (!udev_mon) {
		udev_unref(udev);
		return FALSE;
	}

	if (udev_monitor_filter_add_match_subsystem_devtype(udev_mon,
				   "drm", "drm_minor") ||
		udev_monitor_enable_receiving(udev_mon)) {
		udev_monitor_unref(udev_mon);
		udev_unref(udev);
		return FALSE;
	}

	drm->udev.monitor = udev_mon;
	drm->udev.handler = xf86AddGeneralHandler(udev_monitor_get_fd(udev_mon),
						  common_drm_handle_uevent,
						  pScrn);

	drm->udev.CloseScreen = pScreen->CloseScreen;
	pScreen->CloseScreen = common_drm_udev_CloseScreen;

	return TRUE;
}
#endif

static Bool common_drm_CloseScreen(CLOSE_SCREEN_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	Bool ret;

	if (drm->fb_id) {
		drmModeRmFB(drm->fd, drm->fb_id);
		drm->fb_id = 0;
	}

	if (drm->hw_cursor)
		xf86_cursors_fini(pScreen);

	pScreen->CloseScreen = drm->CloseScreen;
	ret = (*pScreen->CloseScreen)(CLOSE_SCREEN_ARGS);

	if (pScrn->vtSema)
		common_drm_LeaveVT(VT_FUNC_ARGS(0));

	pScrn->vtSema = FALSE;

	return ret;
}

int __common_drm_get_cap(ScrnInfoPtr pScrn, uint64_t cap, uint64_t *val,
	const char *name)
{
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	int err;

	err = drmGetCap(drm->fd, cap, val);
	if (err)
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to get %s capability: %s\n",
			   name, strerror(errno));

	return err;
}

Bool common_drm_PreInit(ScrnInfoPtr pScrn, int flags24)
{
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	rgb defaultWeight = { 0, 0, 0 };
	uint64_t val;
	int depth, bpp;

	pScrn->monitor = pScrn->confScreen->monitor;
	pScrn->progClock = TRUE;
	pScrn->rgbBits = 8;
	pScrn->displayWidth = 640;

	depth = bpp = 0;
	if (drmGetCap(drm->fd, DRM_CAP_DUMB_PREFERRED_DEPTH, &val) == 0) {
		switch (val) {
		case 8:
		case 15:
		case 16:
			bpp = (val + 7) & ~7;
			depth = val;
			break;
		default:
			depth = 24;
			break;
		}
	}

	if (!xf86SetDepthBpp(pScrn, depth, depth, bpp, flags24))
		return FALSE;

	xf86PrintDepthBpp(pScrn);

	if (!xf86SetWeight(pScrn, defaultWeight, defaultWeight))
		return FALSE;

	if (!xf86SetDefaultVisual(pScrn, -1))
		return FALSE;

	return TRUE;
}

Bool common_drm_PreScreenInit(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	int visuals, preferredCVC;

	if (!dixRegisterPrivateKey(&pixmap_key, PRIVATE_PIXMAP,
				   sizeof(struct common_pixmap)))
		return FALSE;

	drm->Options = xnfalloc(sizeof(common_drm_options));
	memcpy(drm->Options, common_drm_options, sizeof(common_drm_options));
	xf86ProcessOptions(pScrn->scrnIndex, pScrn->options, drm->Options);

	miClearVisualTypes();
	if (pScrn->bitsPerPixel > 8) {
		visuals = TrueColorMask;
		preferredCVC = TrueColor;
	} else {
		visuals = miGetDefaultVisualMask(pScrn->depth);
		preferredCVC = pScrn->defaultVisual;
	}

	if (!miSetVisualTypes(pScrn->depth, visuals, pScrn->rgbBits, preferredCVC)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to set visual types for %d bpp depth %d\n",
			   pScrn->bitsPerPixel, pScrn->depth);
		return FALSE;
	}

	if (!miSetPixmapDepths()) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to set pixmap depths\n");
		return FALSE;
	}

	if (!fbScreenInit(pScreen, NULL, pScrn->virtualX, pScrn->virtualY,
			  pScrn->xDpi, pScrn->yDpi, pScrn->displayWidth,
			  pScrn->bitsPerPixel)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] fbScreenInit failed\n");
		return FALSE;
	}

	if (pScrn->bitsPerPixel > 8) {
		/* Fixup RGB ordering */
		VisualPtr visual = pScreen->visuals + pScreen->numVisuals;
		while (--visual >= pScreen->visuals) {
			if ((visual->class | DynamicClass) == DirectColor) {
				visual->offsetRed   = pScrn->offset.red;
				visual->offsetGreen = pScrn->offset.green;
				visual->offsetBlue  = pScrn->offset.blue;
				visual->redMask     = pScrn->mask.red;
				visual->greenMask   = pScrn->mask.green;
				visual->blueMask    = pScrn->mask.blue;
			}
		}
	}

	if (!fbPictureInit(pScreen, NULL, 0)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] fbPictureInit failed\n");
		return FALSE;
	}

	xf86SetBlackWhitePixels(pScreen);

	xf86SetBackingStore(pScreen);
	xf86SetSilkenMouse(pScreen);

	return TRUE;
}

#if HAVE_NOTIFY_FD
static void
drmmode_notify_fd(int fd, int notify, void *data)
{
	struct common_drm_info *drm = data;

	drmHandleEvent(drm->fd, &drm->event_context);
}
#else
static void common_drm_wakeup_handler(pointer data, int err, pointer p)
{
	struct common_drm_info *drm = data;
	fd_set *read_mask = p;

	if (data == NULL || err < 0)
		return;

	if (FD_ISSET(drm->fd, read_mask))
		drmHandleEvent(drm->fd, &drm->event_context);
}
#endif

Bool common_drm_PostScreenInit(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);

#ifdef HAVE_PRESENT
	if (xf86ReturnOptValBool(drm->Options, OPTION_PRESENT, TRUE))
		common_present_init(pScreen);
#endif

	pScreen->SaveScreen = xf86SaveScreen;

	/* software cursor */
	miDCInitialize(pScreen, xf86GetPointerScreenFuncs());

	drm->hw_cursor = xf86ReturnOptValBool(drm->Options,
					      OPTION_HW_CURSOR,
					      drm->has_hw_cursor);
	if (drm->hw_cursor && !drm->has_hw_cursor) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "No hardware cursor support - disabling hardware cursors\n");
		drm->hw_cursor = FALSE;
	}
	if (drm->hw_cursor &&
	    xf86_cursors_init(pScreen,
			      drm->cursor_max_width, drm->cursor_max_height,
			      HARDWARE_CURSOR_TRUECOLOR_AT_8BPP |
			      HARDWARE_CURSOR_BIT_ORDER_MSBFIRST |
			      HARDWARE_CURSOR_INVERT_MASK |
			      HARDWARE_CURSOR_SWAP_SOURCE_AND_MASK |
			      HARDWARE_CURSOR_AND_SOURCE_WITH_MASK |
			      HARDWARE_CURSOR_SOURCE_MASK_INTERLEAVE_64 |
			      HARDWARE_CURSOR_UPDATE_UNHIDDEN |
			      HARDWARE_CURSOR_ARGB)) {
		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
			   "Using hardware cursors\n");
	} else {
		drm->hw_cursor = FALSE;
	}

	drm->CloseScreen = pScreen->CloseScreen;
	pScreen->CloseScreen = common_drm_CloseScreen;

	if (!xf86CrtcScreenInit(pScreen)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to initialize screen\n");
		return FALSE;
	}

	if (!miCreateDefColormap(pScreen)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to initialize default colormap\n");
		return FALSE;
	}

	if (!xf86HandleColormaps(pScreen, 256, 8, common_drm_LoadPalette, NULL,
				 CMAP_RELOAD_ON_MODE_SWITCH |
				 CMAP_PALETTED_TRUECOLOR)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to initialize colormap handler\n");
		return FALSE;
	}

	xf86DPMSInit(pScreen, xf86DPMSSet, 0);

	/* Setup the synchronisation feedback */
#if HAVE_NOTIFY_FD
	SetNotifyFd(drm->fd, drmmode_notify_fd, X_NOTIFY_READ, drm);
#else
	AddGeneralSocket(drm->fd);
	RegisterBlockAndWakeupHandlers((BlockHandlerProcPtr)NoopDDA,
				       common_drm_wakeup_handler, drm);
#endif

#ifdef HAVE_UDEV
	if (!common_drm_udev_init(pScreen)) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "[drm] failed to connect with udev: %s\n",
			   strerror(errno));
		return FALSE;
	}
#endif

	return TRUE;
}

Bool common_drm_SwitchMode(SWITCH_MODE_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);

	return xf86SetSingleMode(pScrn, mode, RR_Rotate_0);
}

void common_drm_AdjustFrame(ADJUST_FRAME_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	xf86OutputPtr output = xf86_config->output[xf86_config->compat_output];
	xf86CrtcPtr crtc = output->crtc;

	if (crtc && crtc->enabled) {
		int saved_x = crtc->x;
		int saved_y = crtc->y;
		int ret;

		crtc->x = x;
		crtc->y = y;

		ret = crtc->funcs->set_mode_major(crtc, &crtc->mode,
						  crtc->rotation, x, y);
		if (!ret) {
			crtc->x = saved_x;
			crtc->y = saved_y;
		}
	}
}

Bool common_drm_EnterVT(VT_FUNC_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	int i;

	if (!common_drm_get_master(drm->dev)) {
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "[drm] set master failed: %s\n", strerror(errno));
		return FALSE;
	}

	if (!xf86SetDesiredModes(pScrn)) {
		common_drm_put_master(drm->dev);
		return FALSE;
	}

	/* Disable unused CRTCs */
	for (i = 0; i < xf86_config->num_crtc; i++) {
		xf86CrtcPtr crtc = xf86_config->crtc[i];
		struct common_crtc_info *drmc = common_crtc(crtc);

		if (!crtc->enabled)
			drmModeSetCrtc(drmc->drm_fd, drmc->drm_id,
				       0, 0, 0, NULL, 0, NULL);
	}

	return TRUE;
}

void common_drm_LeaveVT(VT_FUNC_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);

	xf86RotateFreeShadow(pScrn);

	xf86_hide_cursors(pScrn);

	common_drm_put_master(drm->dev);
}

void common_drm_FreeScreen(FREE_SCREEN_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);

	if (drm) {
		drmClose(drm->fd);
		SET_DRM_INFO(pScrn, NULL);
		free(drm);
	}
}

/*
 * Helpers for DRI2 and textured Xv
 */
_X_EXPORT
xf86CrtcPtr common_drm_covering_crtc(ScrnInfoPtr pScrn, BoxPtr box,
	xf86CrtcPtr desired, BoxPtr box_ret)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	xf86CrtcPtr crtc, best_crtc;
	BoxRec crtc_box, cover_box;
	int coverage, best_coverage, c;

	/* If we do not own the VT, we do not own the CRTC */
	if (!pScrn->vtSema)
		return NULL;

	best_crtc = NULL;
	best_coverage = 0;
	box_init(box_ret, 0, 0, 0, 0);
	for (c = 0; c < xf86_config->num_crtc; c++) {
		crtc = xf86_config->crtc[c];
		if (!crtc->enabled)
			continue;
		box_init(&crtc_box, crtc->x, crtc->y,
		         xf86ModeWidth(&crtc->mode, crtc->rotation),
			 xf86ModeHeight(&crtc->mode, crtc->rotation));
		box_intersect(&cover_box, &crtc_box, box);
		coverage = box_area(&cover_box);
		if (coverage && crtc == desired) {
			*box_ret = crtc_box;
			return crtc;
		} else if (coverage > best_coverage) {
			*box_ret = crtc_box;
			best_crtc = crtc;
			best_coverage = coverage;
		}
	}
	return best_crtc;
}

_X_EXPORT
xf86CrtcPtr common_drm_drawable_covering_crtc(DrawablePtr pDraw)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pDraw->pScreen);
	xf86CrtcPtr crtc;
	BoxRec box, crtcbox;

	box_init(&box, pDraw->x, pDraw->y, pDraw->width, pDraw->height);

	crtc = common_drm_covering_crtc(pScrn, &box, NULL, &crtcbox);

	/* Make sure the CRTC is valid and this is the real front buffer */
	if (crtc && crtc->rotatedData)
		crtc = NULL;

	return crtc;
}

static inline uint32_t req_crtc(xf86CrtcPtr crtc)
{
	struct common_crtc_info *drmc = common_crtc(crtc);

	/*
	 * We only support newer kernels here - always
	 * encode the CRTC id in the high crtc field.
	 */
	return drmc->num << DRM_VBLANK_HIGH_CRTC_SHIFT;
}

_X_EXPORT
int common_drm_vblank_get(ScrnInfoPtr pScrn, xf86CrtcPtr crtc,
	drmVBlank *vbl, const char *func)
{
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);

	vbl->request.type = DRM_VBLANK_RELATIVE | req_crtc(crtc);
	vbl->request.sequence = 0;

	return drmWaitVBlank(drm->fd, vbl);
}

_X_EXPORT
int common_drm_get_msc(xf86CrtcPtr crtc, uint64_t *ust, uint64_t *msc)
{
	struct common_crtc_info *drmc = common_crtc(crtc);
	drmVBlank vbl;
	int ret;

	ret = common_drm_vblank_get(crtc->scrn, crtc, &vbl, __FUNCTION__);
	if (ret == 0) {
		drmc->swap_msc = common_drm_frame_to_msc(crtc,
							 vbl.reply.sequence);
		drmc->swap_ust = ((CARD64)vbl.reply.tval_sec * 1000000) +
				 vbl.reply.tval_usec;
	}

	*ust = drmc->swap_ust;
	*msc = drmc->swap_msc;

	return Success;
}

_X_EXPORT
int common_drm_get_drawable_msc(xf86CrtcPtr crtc, DrawablePtr pDraw,
	uint64_t *ust, uint64_t *msc)
{
	struct common_pixmap *drawc;
	int ret = Success;

	if (!pDraw && !crtc) {
		*ust = *msc = 0;
		return Success;
	}

	if (!pDraw)
		return common_drm_get_msc(crtc, ust, msc);

	drawc = common_drm_pixmap(drawable_pixmap(pDraw));

	if (drawc->crtc) {
		uint64_t old_ust, old_msc;

		ret = common_drm_get_msc(drawc->crtc, &old_ust, &old_msc);
		if (ret == Success) {
			drawc->last_ust = old_ust;
			drawc->last_msc = old_msc + drawc->delta_msc;
		}
	}

	if (drawc->crtc != crtc) {
		uint64_t new_ust, new_msc;

		drawc->crtc = crtc;

		if (crtc) {
			ret = common_drm_get_msc(crtc, &new_ust, &new_msc);
			if (ret == Success)
				drawc->delta_msc = drawc->last_msc - new_msc;
		}
	}

	*ust = drawc->last_ust;
	*msc = drawc->last_msc;

	return ret;
}

_X_EXPORT
int common_drm_queue_msc_event(ScrnInfoPtr pScrn, xf86CrtcPtr crtc,
	uint64_t *msc, const char *func, Bool nextonmiss,
	struct common_drm_event *event)
{
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	drmVBlank vbl;
	int ret;

	vbl.request.type = DRM_VBLANK_ABSOLUTE | DRM_VBLANK_EVENT |
				req_crtc(crtc);
	vbl.request.sequence = common_drm_msc_to_frame(crtc, *msc);
	vbl.request.signal = (unsigned long)event;

	if (nextonmiss)
		vbl.request.type |= DRM_VBLANK_NEXTONMISS;

	ret = drmWaitVBlank(drm->fd, &vbl);
	if (ret)
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "%s: %s failed: %s\n", func,
			   __FUNCTION__, strerror(errno));
	else
		*msc = common_drm_frame_to_msc(crtc, vbl.reply.sequence);

	return ret;
}

_X_EXPORT
int common_drm_queue_drawable_msc_event(ScrnInfoPtr pScrn, xf86CrtcPtr crtc,
	DrawablePtr pDraw, uint64_t *pmsc, const char *func, Bool nextonmiss,
	struct common_drm_event *event)
{
	struct common_pixmap *drawc;
	uint64_t msc = *pmsc;
	int64_t delta = 0;
	int ret;

	/*
	 * If we have a drawable, we need to correct the MSC for the
	 * drawable delta.
	 */
	if (pDraw) {
		drawc = common_drm_pixmap(drawable_pixmap(pDraw));
		delta = drawc->delta_msc;
		msc -= delta;
	}

	ret = common_drm_queue_msc_event(pScrn, crtc, &msc, func, nextonmiss,
					 event);

	*pmsc = msc + delta;

	return ret;
}

_X_EXPORT
int common_drm_vblank_wait(ScrnInfoPtr pScrn, xf86CrtcPtr crtc,
	drmVBlank *vbl, const char *func, Bool nextonmiss)
{
	struct common_drm_info *drm = GET_DRM_INFO(pScrn);
	int ret;

	vbl->request.type = DRM_VBLANK_ABSOLUTE | req_crtc(crtc);

	if (nextonmiss)
		vbl->request.type |= DRM_VBLANK_NEXTONMISS;

	ret = drmWaitVBlank(drm->fd, vbl);
	if (ret)
		xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
			   "%s: %s failed: %s\n", func,
			   __FUNCTION__, strerror(errno));

	return ret;
}


static int common_entity_key = -1;

struct common_drm_device *common_entity_get_dev(int entity_num)
{
	if (common_entity_key == -1)
		common_entity_key = xf86AllocateEntityPrivateIndex();
	if (common_entity_key == -1)
		return NULL;

	return xf86GetEntityPrivate(entity_num, common_entity_key)->ptr;
}

static void common_entity_set_dev(int entity_num, struct common_drm_device *dev)
{
	if (common_entity_key == -1)
		common_entity_key = xf86AllocateEntityPrivateIndex();

	xf86GetEntityPrivate(entity_num, common_entity_key)->ptr = dev;
}

struct common_drm_device *common_alloc_dev(int entity_num, int fd,
	const char *path, Bool ddx_managed_master)
{
	struct common_drm_device *drm_dev;

	drm_dev = malloc(sizeof *drm_dev);
	if (!drm_dev)
		return NULL;

	drm_dev->fd = fd;
	drm_dev->master_count = !ddx_managed_master;

	if (path) {
		drm_dev->kms_path = strdup(path);
		if (!drm_dev->kms_path) {
			free(drm_dev);
			return NULL;
		}
	} else {
		drm_dev->kms_path = NULL;
	}

	common_entity_set_dev(entity_num, drm_dev);

	return drm_dev;
}

/*
 * Check that what we opened was a master or a master-capable FD
 * by setting the version of the interface we'll use to talk to it.
 */
Bool common_drm_fd_is_master(int fd)
{
	drmSetVersion sv;

	sv.drm_di_major = 1;
	sv.drm_di_minor = 1;
	sv.drm_dd_major = -1;
	sv.drm_dd_minor = -1;

	return drmSetInterfaceVersion(fd, &sv) == 0;
}

Bool common_drm_get_master(struct common_drm_device *drm_dev)
{
	if (drm_dev->master_count++)
		return TRUE;

	return drmSetMaster(drm_dev->fd) ? FALSE : TRUE;
}

void common_drm_put_master(struct common_drm_device *drm_dev)
{
	assert(drm_dev->master_count);

	if (--drm_dev->master_count == 0)
		drmDropMaster(drm_dev->fd);
}
