/**
 * @file display.c
 * Display module -- this implements display handling for MCE
 * <p>
 * Copyright © 2007-2010 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 *
 * mce is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * mce is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mce.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <glib.h>
#include <gmodule.h>
#include <glib/gstdio.h>		/* g_access() */

#include <errno.h>			/* errno */
#include <fcntl.h>			/* open() */
#include <stdio.h>			/* O_RDWR */
#include <string.h>			/* strcmp() */
#include <unistd.h>			/* close() */
#include <linux/fb.h>			/* FBIOBLANK,
					 * FB_BLANK_POWERDOWN,
					 * FB_BLANK_UNBLANK
					 */
#include <sys/ioctl.h>			/* ioctl() */

#include <mce/mode-names.h>		/* MCE_CABC_MODE_OFF,
					 * MCE_CABC_MODE_UI,
					 * MCE_CABC_MODE_STILL_IMAGE,
					 * MCE_CABC_MODE_MOVING_IMAGE,
					 * MCE_DISPLAY_ON_STRING,
					 * MCE_DISPLAY_DIM_STRING,
					 * MCE_DISPLAY_OFF_STRING
					 */

#include "mce.h"			/* display_state_t,
					 * charger_state_pipe,
					 * display_state_pipe,
					 * display_brightness_pipe,
					 * inactivity_timeout_pipe,
					 * led_pattern_deactivate_pipe,
					 * submode_pipe,
					 * system_state_pipe,
					 * device_inactive_pipe
					 */
#include "display.h"

#include "mce-io.h"			/* mce_read_string_from_file(),
					 * mce_read_number_string_from_file(),
					 * mce_write_number_string_to_file()
					 */
#include "mce-lib.h"			/* strstr_delim(),
					 * mce_translate_string_to_int_with_default(),
					 * mce_translation_t
					 */
#include "mce-log.h"			/* mce_log(), LL_* */
#include "mce-conf.h"			/* mce_conf_get_int(),
					 * mce_conf_get_string()
					 */
#include "mce-dbus.h"			/* Direct:
					 * ---
					 * mce_dbus_handler_add(),
					 * dbus_send_message(),
					 * dbus_new_method_reply(),
					 * dbus_new_signal(),
					 * dbus_message_append_args(),
					 * dbus_message_get_no_reply(),
					 * dbus_message_unref(),
					 * DBusMessage,
					 * DBUS_MESSAGE_TYPE_METHOD_CALL,
					 * DBUS_MESSAGE_TYPE_SIGNAL,
					 * DBUS_TYPE_STRING,
					 * DBUS_TYPE_INVALID,
					 * dbus_bool_t
					 *
					 * Indirect:
					 * ---
					 * MCE_SIGNAL_IF,
					 * MCE_SIGNAL_PATH,
					 * MCE_REQUEST_IF,
					 * MCE_DISPLAY_STATUS_GET,
					 * MCE_DISPLAY_ON_REQ,
					 * MCE_DISPLAY_DIM_REQ,
					 * MCE_DISPLAY_OFF_REQ,
					 * MCE_PREVENT_BLANK_REQ,
					 * MCE_CABC_MODE_GET,
					 * MCE_CABC_MODE_REQ
					 */
#include "mce-gconf.h"			/* mce_gconf_get_int(),
					 * mce_gconf_get_bool(),
					 * mce_gconf_notifier_add(),
					 * gconf_entry_get_key(),
					 * gconf_value_get_int(),
					 * gconf_value_get_bool(),
					 * GConfClient, GConfEntry, GConfValue
					 */
#include "datapipe.h"			/* datapipe_get_gint(),
					 * execute_datapipe(),
					 * append_output_trigger_to_datapipe(),
					 * remove_output_trigger_from_datapipe()
					 */

/* These defines are taken from devicelock.h, but slightly modified */
#ifndef DEVICELOCK_H
/** Devicelock D-Bus service */
#define DEVLOCK_SERVICE			"com.nokia.devicelock"

/** Devicelock D-Bus service */
#define DEVLOCK_PATH			"/request"

/** Set devicelock state */
#define DEVLOCK_SET			"setState"

/** Enumeration of the valid locks on the device */
enum LockType {
	/** TouchAndKeyboard -- The touch screen and keypad lock */
	TouchAndKeyboard = 0,
	/** Device -- The device lock, password protected lock screen */
	Device
};

/** Enumeration of the valid states that a lock can be in */
enum LockState {
	/** Unlocked - The lock is unlocked */
	Unlocked = 0,
	/** Locked - The lock is being used */
	Locked,
	/** Configuration - Open the locks configuration settings */
	Configuration,
/** WipeMMC - Secure wipe of the device */
	WipeMMC,
	/** Inhibit - Stop the lock ui(s) from being displayed */
	Inhibit,
	/** Undefined - Lock state is unknown or the lock does not exist */
	Undefined
};
#endif /* DEVICELOCK_H */

// XXX: remove
#ifndef MCE_DISPLAY_LOW_POWER_MODE_REQ
#define MCE_DISPLAY_LOW_POWER_MODE_REQ	"req_display_state_low_power"
#endif

/** Module name */
#define MODULE_NAME		"display"

/** Functionality provided by this module */
static const gchar *const provides[] = { MODULE_NAME, NULL };

/** Module information */
G_MODULE_EXPORT module_info_struct module_info = {
	/** Name of the module */
	.name = MODULE_NAME,
	/** Module provides */
	.provides = provides,
	/** Module priority */
	.priority = 250
};

/** GConf callback ID for display brightness setting */
static guint disp_brightness_gconf_cb_id = 0;

/** Display dimming timeout setting */
static gint disp_dim_timeout = DEFAULT_DIM_TIMEOUT;
/** GConf callback ID for display dimming timeout setting */
static guint disp_dim_timeout_gconf_cb_id = 0;

/** Display blanking timeout setting */
static gint disp_blank_timeout = DEFAULT_BLANK_TIMEOUT;
/** GConf callback ID for display blanking timeout setting */
static guint disp_blank_timeout_gconf_cb_id = 0;

/** ID for display blank prevention timer source */
static guint blank_prevent_timeout_cb_id = 0;

/** GConf callback ID for display blanking timeout setting */
static guint adaptive_dimming_enabled_gconf_cb_id = 0;

/** ID for adaptive display dimming timer source */
static guint adaptive_dimming_timeout_cb_id = 0;

/** Use adaptive timeouts for dimming */
static gboolean adaptive_dimming_enabled = DEFAULT_ADAPTIVE_DIMMING_ENABLED;

/** GConf callback ID for the threshold for adaptive display dimming */
static guint adaptive_dimming_threshold_gconf_cb_id = 0;

/** Threshold to use for adaptive timeouts for dimming in milliseconds */
static gint adaptive_dimming_threshold = DEFAULT_ADAPTIVE_DIMMING_THRESHOLD;

/** Display blank prevention timer */
static gint blank_prevent_timeout = BLANK_PREVENT_TIMEOUT;

/** Bootup dim additional timeout */
static gint bootup_dim_additional_timeout = 0;

/** Cached brightness */
static gint cached_brightness = -1;

/** Target brightness */
static gint target_brightness = -1;

/** Brightness */
static gint set_brightness = -1;

/** Dim brightness */
static gint dim_brightness = (DEFAULT_MAXIMUM_DISPLAY_BRIGHTNESS *
			      DEFAULT_DIM_BRIGHTNESS) / 100;

/** CABC mode -- uses the SysFS mode names */
static const gchar *cabc_mode = DEFAULT_CABC_MODE;
/**
 * CABC mode (power save mode active) -- uses the SysFS mode names;
 * NULL to disable
 */
static const gchar *psm_cabc_mode = NULL;

/** Fadeout step length */
static gint brightness_fade_steplength = 2;

/** Brightness fade timeout callback ID */
static guint brightness_fade_timeout_cb_id = 0;
/** Display dimming timeout callback ID */
static guint dim_timeout_cb_id = 0;
/** Display blanking timeout callback ID */
static guint blank_timeout_cb_id = 0;

/** Charger state */
static gboolean charger_connected = FALSE;

/** Maximum display brightness */
static gint maximum_display_brightness = DEFAULT_MAXIMUM_DISPLAY_BRIGHTNESS;

/** File used to set display brightness */
static gchar *brightness_file = NULL;
/** File pointer used to set display brightness */
static FILE *brightness_fp = NULL;
/** File used to get maximum display brightness */
static gchar *max_brightness_file = NULL;
/** File used to set the CABC mode */
static gchar *cabc_mode_file = NULL;
/** File used to get the available CABC modes */
static gchar *cabc_available_modes_file = NULL;
/** Is hardware driven display fading supported */
static gboolean hw_display_fading_supported = FALSE;
/** Is display low power mode supported */
static gboolean display_low_power_mode_supported = FALSE;

/** Brightness change policies */
typedef enum {
	/** Policy not set */
	BRIGHTNESS_CHANGE_POLICY_INVALID = MCE_INVALID_TRANSLATION,
	/** Brightness changes instantly */
	BRIGHTNESS_CHANGE_DIRECT = 0,
	/** Fade with fixed step time */
	BRIGHTNESS_CHANGE_STEP_TIME = 1,
	/** Fade time independent of number of steps faded */
	BRIGHTNESS_CHANGE_CONSTANT_TIME = 2,
	/** Default setting when brightness increases */
	DEFAULT_BRIGHTNESS_INCREASE_POLICY = BRIGHTNESS_CHANGE_CONSTANT_TIME,
	/** Default setting when brightness decreases */
	DEFAULT_BRIGHTNESS_DECREASE_POLICY = BRIGHTNESS_CHANGE_CONSTANT_TIME
} brightness_change_policy_t;

/** Mapping of brightness change integer <-> policy string */
static const mce_translation_t brightness_change_policy_translation[] = {
	{
		.number = BRIGHTNESS_CHANGE_DIRECT,
		.string = "direct",
	}, {
		.number = BRIGHTNESS_CHANGE_STEP_TIME,
		.string = "steptime",
	}, {
		.number = BRIGHTNESS_CHANGE_CONSTANT_TIME,
		.string = "constanttime",
	}, { /* MCE_INVALID_TRANSLATION marks the end of this array */
		.number = MCE_INVALID_TRANSLATION,
		.string = NULL
	}
};

/** Real display brightness setting */
static gint real_disp_brightness = DEFAULT_DISP_BRIGHTNESS;

/** Brightness increase policy */
static brightness_change_policy_t brightness_increase_policy =
					DEFAULT_BRIGHTNESS_INCREASE_POLICY;

/** Brightness decrease policy */
static brightness_change_policy_t brightness_decrease_policy =
					DEFAULT_BRIGHTNESS_DECREASE_POLICY;

/** Brightness increase step-time */
static gint brightness_increase_step_time =
		DEFAULT_BRIGHTNESS_INCREASE_STEP_TIME;

/** Brightness decrease step-time */
static gint brightness_decrease_step_time =
		DEFAULT_BRIGHTNESS_DECREASE_STEP_TIME;

/** Brightness increase constant time */
static gint brightness_increase_constant_time =
		DEFAULT_BRIGHTNESS_INCREASE_CONSTANT_TIME;

/** Brightness decrease constant time */
static gint brightness_decrease_constant_time =
		DEFAULT_BRIGHTNESS_DECREASE_CONSTANT_TIME;

/**
 * Display brightness setting (power save mode active);
 * -1 to disable
 */
static gint psm_disp_brightness = -1;

/** Inhibit type */
typedef enum {
	/** Inhibit value invalid */
	INHIBIT_INVALID = -1,
	/** No inhibit */
	INHIBIT_OFF = 0,
	/** Default value */
	DEFAULT_BLANKING_INHIBIT_MODE = INHIBIT_OFF,
	/** Inhibit blanking; always keep on if charger connected */
	INHIBIT_STAY_ON_WITH_CHARGER = 1,
	/** Inhibit blanking; always keep on or dimmed if charger connected */
	INHIBIT_STAY_DIM_WITH_CHARGER = 2,
	/** Inhibit blanking; always keep on */
	INHIBIT_STAY_ON = 3,
	/** Inhibit blanking; always keep on or dimmed */
	INHIBIT_STAY_DIM = 4,
} inhibit_t;

/** Display blanking inhibit mode */
static inhibit_t blanking_inhibit_mode = DEFAULT_BLANKING_INHIBIT_MODE;
/** GConf callback ID for display blanking inhibit mode setting */
static guint blanking_inhibit_mode_gconf_cb_id = 0;

/** Blanking inhibited */
static gboolean blanking_inhibited = FALSE;
/** Dimming inhibited */
static gboolean dimming_inhibited = FALSE;

/** List of monitored blanking pause requesters */
static GSList *blanking_pause_monitor_list = NULL;

/** List of monitored CABC mode requesters */
static GSList *cabc_mode_monitor_list = NULL;

static void update_blanking_inhibit(gboolean timed_inhibit);
static void cancel_dim_timeout(void);

/** Display type */
typedef enum {
	/** Display type unset */
	DISPLAY_TYPE_UNSET = -1,
	/** No display available; XXX should never happen */
	DISPLAY_TYPE_NONE = 0,
	/** Generic display interface without CABC */
	DISPLAY_TYPE_GENERIC = 1,
	/** EID l4f00311 with CABC */
	DISPLAY_TYPE_L4F00311 = 2,
	/** Sony acx565akm with CABC */
	DISPLAY_TYPE_ACX565AKM = 3,
	/** Taal display */
	DISPLAY_TYPE_TAAL = 4,
	/** Himalaya display */
	DISPLAY_TYPE_HIMALAYA = 5,
	/** Generic display name */
	DISPLAY_TYPE_DISPLAY0 = 6,
	/** Generic name for ACPI-controlled displays */
	DISPLAY_TYPE_ACPI_VIDEO0 = 7
} display_type_t;

/**
 * Array of possible display dim timeouts
 */
static GSList *possible_dim_timeouts = NULL;

/**
 * Index for the array of possible display dim timeouts
 */
static guint dim_timeout_index = 0;

/**
 * Index for the array of adaptive dimming timeout multipliers
 */
static guint adaptive_dimming_index = 0;

/**
 * CABC mapping; D-Bus API modes vs SysFS mode
 */
typedef struct {
	/** CABC mode D-Bus name */
	const gchar *const dbus;
	/** CABC mode SysFS name */
	const gchar *const sysfs;
	/** CABC mode available */
	gboolean available;
} cabc_mode_mapping_t;

/**
 * CABC mappings; D-Bus API modes vs SysFS mode
 */
cabc_mode_mapping_t cabc_mode_mapping[] = {
	{
		.dbus = MCE_CABC_MODE_OFF,
		.sysfs = CABC_MODE_OFF,
		.available = FALSE
	}, {
		.dbus = MCE_CABC_MODE_UI,
		.sysfs = CABC_MODE_UI,
		.available = FALSE
	}, {
		.dbus = MCE_CABC_MODE_STILL_IMAGE,
		.sysfs = CABC_MODE_STILL_IMAGE,
		.available = FALSE
	}, {
		.dbus = MCE_CABC_MODE_MOVING_IMAGE,
		.sysfs = CABC_MODE_MOVING_IMAGE,
		.available = FALSE
	}, {
		.dbus = NULL,
		.sysfs = NULL,
		.available = FALSE
	}
};

/**
 * Get the display type
 *
 * @return The display type
 */
static display_type_t get_display_type(void)
{
	static display_type_t display_type = DISPLAY_TYPE_UNSET;

	/* If we have the display type already, return it */
	if (display_type != DISPLAY_TYPE_UNSET)
		goto EXIT;

	if (g_access(DISPLAY_CABC_PATH DISPLAY_ACX565AKM, W_OK) == 0) {
		display_type = DISPLAY_TYPE_ACX565AKM;

		brightness_file = g_strconcat(DISPLAY_CABC_PATH, DISPLAY_ACX565AKM, DISPLAY_CABC_BRIGHTNESS_FILE, NULL);
		max_brightness_file = g_strconcat(DISPLAY_CABC_PATH, DISPLAY_ACX565AKM, DISPLAY_CABC_MAX_BRIGHTNESS_FILE, NULL);
		cabc_mode_file = g_strconcat(DISPLAY_CABC_PATH, DISPLAY_ACX565AKM, DISPLAY_CABC_MODE_FILE, NULL);
		cabc_available_modes_file = g_strconcat(DISPLAY_CABC_PATH, DISPLAY_ACX565AKM, DISPLAY_CABC_AVAILABLE_MODES_FILE, NULL);
	} else if (g_access(DISPLAY_CABC_PATH DISPLAY_L4F00311, W_OK) == 0) {
		display_type = DISPLAY_TYPE_L4F00311;

		brightness_file = g_strconcat(DISPLAY_CABC_PATH, DISPLAY_L4F00311, DISPLAY_CABC_BRIGHTNESS_FILE, NULL);
		max_brightness_file = g_strconcat(DISPLAY_CABC_PATH, DISPLAY_L4F00311, DISPLAY_CABC_MAX_BRIGHTNESS_FILE, NULL);
		cabc_mode_file = g_strconcat(DISPLAY_CABC_PATH, DISPLAY_L4F00311, DISPLAY_CABC_MODE_FILE, NULL);
		cabc_available_modes_file = g_strconcat(DISPLAY_CABC_PATH, DISPLAY_L4F00311, DISPLAY_CABC_AVAILABLE_MODES_FILE, NULL);
	} else if (g_access(DISPLAY_CABC_PATH DISPLAY_TAAL, W_OK) == 0) {
		display_type = DISPLAY_TYPE_TAAL;

		brightness_file = g_strconcat(DISPLAY_CABC_PATH, DISPLAY_TAAL, DISPLAY_CABC_BRIGHTNESS_FILE, NULL);
		max_brightness_file = g_strconcat(DISPLAY_CABC_PATH, DISPLAY_TAAL, DISPLAY_CABC_MAX_BRIGHTNESS_FILE, NULL);

		cabc_mode_file = g_strconcat(DISPLAY_CABC_PATH, DISPLAY_TAAL, "/device", DISPLAY_CABC_MODE_FILE, NULL);
		cabc_available_modes_file = g_strconcat(DISPLAY_CABC_PATH, DISPLAY_TAAL, "/device", DISPLAY_CABC_AVAILABLE_MODES_FILE, NULL);
	} else if (g_access(DISPLAY_CABC_PATH DISPLAY_HIMALAYA, W_OK) == 0) {
		display_type = DISPLAY_TYPE_HIMALAYA;

		brightness_file = g_strconcat(DISPLAY_CABC_PATH, DISPLAY_HIMALAYA, DISPLAY_CABC_BRIGHTNESS_FILE, NULL);
		max_brightness_file = g_strconcat(DISPLAY_CABC_PATH, DISPLAY_HIMALAYA, DISPLAY_CABC_MAX_BRIGHTNESS_FILE, NULL);

		cabc_mode_file = g_strconcat(DISPLAY_CABC_PATH, DISPLAY_HIMALAYA, "/device", DISPLAY_CABC_MODE_FILE, NULL);
		cabc_available_modes_file = g_strconcat(DISPLAY_CABC_PATH, DISPLAY_HIMALAYA, "/device", DISPLAY_CABC_AVAILABLE_MODES_FILE, NULL);
	} else if (g_access(DISPLAY_CABC_PATH DISPLAY_DISPLAY0, W_OK) == 0) {
		display_type = DISPLAY_TYPE_DISPLAY0;

		brightness_file = g_strconcat(DISPLAY_CABC_PATH, DISPLAY_DISPLAY0, DISPLAY_CABC_BRIGHTNESS_FILE, NULL);
		max_brightness_file = g_strconcat(DISPLAY_CABC_PATH, DISPLAY_DISPLAY0, DISPLAY_CABC_MAX_BRIGHTNESS_FILE, NULL);

		cabc_mode_file = g_strconcat(DISPLAY_CABC_PATH, DISPLAY_DISPLAY0, "/device", DISPLAY_CABC_MODE_FILE, NULL);
		cabc_available_modes_file = g_strconcat(DISPLAY_CABC_PATH, DISPLAY_DISPLAY0, "/device", DISPLAY_CABC_AVAILABLE_MODES_FILE, NULL);
		hw_display_fading_supported = (g_access(DISPLAY_HARDWARE_DIMMING, W_OK) == 0);

#if 0
		/* Enable hardware fading if supported */
		if (hw_display_fading_supported == TRUE)
			(void)mce_write_number_string_to_file(DISPLAY_HARDWARE_DIMMING, 1, NULL, TRUE, TRUE);
#endif
	} else if (g_access(DISPLAY_CABC_PATH DISPLAY_ACPI_VIDEO0, W_OK) == 0) {
		display_type = DISPLAY_TYPE_ACPI_VIDEO0;

		brightness_file = g_strconcat(DISPLAY_CABC_PATH, DISPLAY_ACPI_VIDEO0, DISPLAY_CABC_BRIGHTNESS_FILE, NULL);
		max_brightness_file = g_strconcat(DISPLAY_CABC_PATH, DISPLAY_ACPI_VIDEO0, DISPLAY_CABC_MAX_BRIGHTNESS_FILE, NULL);
	} else if (g_access(DISPLAY_GENERIC_PATH, W_OK) == 0) {
		display_type = DISPLAY_TYPE_GENERIC;

		brightness_file = g_strconcat(DISPLAY_GENERIC_PATH, DISPLAY_GENERIC_BRIGHTNESS_FILE, NULL);
		max_brightness_file = g_strconcat(DISPLAY_GENERIC_PATH, DISPLAY_GENERIC_MAX_BRIGHTNESS_FILE, NULL);
	} else {
		display_type = DISPLAY_TYPE_NONE;
	}

	mce_log(LL_DEBUG, "Display type: %d", display_type);

EXIT:
	return display_type;
}

/**
 * Set CABC mode
 *
 * @param mode The CABC mode to set
 */
static void set_cabc_mode(const gchar *const mode)
{
	static gboolean available_modes_scanned = FALSE;
	const gchar *tmp = NULL;
	gint i;

	if (cabc_available_modes_file == NULL)
		goto EXIT;

	/* Update the list of available modes against the list we support */
	if (available_modes_scanned == FALSE) {
		gchar *available_modes = NULL;

		available_modes_scanned = TRUE;

		if (mce_read_string_from_file(cabc_available_modes_file,
					      &available_modes) == FALSE)
			goto EXIT;

		for (i = 0; (tmp = cabc_mode_mapping[i].sysfs) != NULL; i++) {
			if (strstr_delim(available_modes, tmp, " ") != NULL)
				cabc_mode_mapping[i].available = TRUE;
		}

		g_free(available_modes);
	}

	/* If the requested mode is supported, use it */
	for (i = 0; (tmp = cabc_mode_mapping[i].sysfs) != NULL; i++) {
		if (cabc_mode_mapping[i].available == FALSE)
			continue;

		if (!strcmp(tmp, mode)) {
			mce_write_string_to_file(cabc_mode_file, tmp);

			/* Don't overwrite the regular CABC mode with the
			 * power save mode CABC mode
			 */
			if (psm_cabc_mode == NULL)
				cabc_mode = tmp;

			break;
		}
	}

EXIT:
	return;
}

/**
 * Call the FBIOBLANK ioctl
 *
 * @param value The ioctl value to pass to the backlight
 * @return TRUE on success, FALSE on failure
 */
static gboolean backlight_ioctl(int value)
{
	static int old_value = FB_BLANK_UNBLANK;
	static int fd = -1;
	gboolean status = FALSE;

	if (fd == -1) {
		if ((fd = open(FB_DEVICE, O_RDWR)) == -1) {
			mce_log(LL_CRIT,
				"Failed to open `%s'; %s",
				FB_DEVICE, g_strerror(errno));

			/* Reset errno,
			 * to avoid false positives down the line
			 */
			errno = 0;
			goto EXIT;
		}

		old_value = !value; /* force ioctl() */
	}

	if (value != old_value) {
		if (ioctl(fd, FBIOBLANK, value) == -1) {
			mce_log(LL_CRIT,
				"ioctl() FBIOBLANK (%d) failed on `%s'; %s",
				value, FB_DEVICE, g_strerror(errno));

			/* Reset errno,
			 * to avoid false positives down the line
			 */
			errno = 0;

			if (close(fd) == -1) {
				mce_log(LL_ERR,
					"Failed to close `%s': %s",
					FB_DEVICE, g_strerror(errno));

				/* Reset errno,
				 * to avoid false positives down the line
				 */
				errno = 0;
			}

			fd = -1;
			goto EXIT;
		}

		old_value = value;
	}

	status = TRUE;

EXIT:
	return status;
}

/**
 * Timeout callback for the brightness fade
 *
 * @param data Unused
 * @return Returns TRUE to repeat, until the cached brightness has reached
 *         the destination value; when this happens, FALSE is returned
 */
static gboolean brightness_fade_timeout_cb(gpointer data)
{
	gboolean retval = TRUE;

	(void)data;

	if ((cached_brightness <= 0) && (target_brightness != 0)) {
		backlight_ioctl(FB_BLANK_UNBLANK);
	}

	if ((cached_brightness == -1) ||
	    (ABS(cached_brightness -
		 target_brightness) < brightness_fade_steplength)) {
		cached_brightness = target_brightness;
		retval = FALSE;
	} else if (target_brightness > cached_brightness) {
		cached_brightness += brightness_fade_steplength;
	} else {
		cached_brightness -= brightness_fade_steplength;
	}

	mce_write_number_string_to_file(brightness_file,
					cached_brightness,
					&brightness_fp, TRUE, FALSE);

	if (cached_brightness == 0) {
		backlight_ioctl(FB_BLANK_POWERDOWN);
	}

	if (retval == FALSE)
		 brightness_fade_timeout_cb_id = 0;

	return retval;
}

/**
 * Cancel the brightness fade timeout
 */
static void cancel_brightness_fade_timeout(void)
{
	/* Remove the timeout source for the display brightness fade */
	if (brightness_fade_timeout_cb_id != 0) {
		g_source_remove(brightness_fade_timeout_cb_id);
		brightness_fade_timeout_cb_id = 0;
	}
}

/**
 * Setup the brightness fade timeout
 *
 * @param step_time The time between each brightness step
 */
static void setup_brightness_fade_timeout(gint step_time)
{
	cancel_brightness_fade_timeout();

	/* Setup new timeout */
	brightness_fade_timeout_cb_id =
		g_timeout_add(step_time, brightness_fade_timeout_cb, NULL);
}

/**
 * Update brightness fade
 *
 * Will fade from current value to new value
 *
 * @param new_brightness The new brightness to fade to
 */
static void update_brightness_fade(gint new_brightness)
{
	gboolean increase = (new_brightness >= cached_brightness);
	gint step_time = 10;

	/* This should never happen, but just in case */
	if (cached_brightness == new_brightness)
		goto EXIT;

	/* If we have support for HW-fading, or if we're using the direct
	 * brightness change policy, don't bother with any of this
	 */
	if ((hw_display_fading_supported == TRUE) ||
	    ((brightness_increase_policy == BRIGHTNESS_CHANGE_DIRECT) &&
	     (increase == TRUE)) ||
	    ((brightness_decrease_policy == BRIGHTNESS_CHANGE_DIRECT) &&
	     (increase == FALSE))) {
		cancel_brightness_fade_timeout();
		cached_brightness = new_brightness;
		target_brightness = new_brightness;
		backlight_ioctl(FB_BLANK_UNBLANK);
		mce_write_number_string_to_file(brightness_file,
						new_brightness,
						&brightness_fp, TRUE, FALSE);
		goto EXIT;
	}

	/* If we're already fading towards the right brightness,
	 * don't change anything
	 */
	if (target_brightness == new_brightness)
		goto EXIT;

	target_brightness = new_brightness;

	if (increase == TRUE) {
		if (brightness_increase_policy == BRIGHTNESS_CHANGE_STEP_TIME)
			step_time = brightness_increase_step_time;
		else {
			step_time = brightness_increase_constant_time /
				    (new_brightness - cached_brightness);
		}
	} else {
		if (brightness_decrease_policy == BRIGHTNESS_CHANGE_STEP_TIME)
			step_time = brightness_decrease_step_time;
		else {
			step_time = brightness_decrease_constant_time /
				    (cached_brightness - new_brightness);
		}
	}

	/* Special case */
	if (step_time == 5) {
		step_time = 2;
		brightness_fade_steplength = 2;
	} else {
		brightness_fade_steplength = 1;
	}

	setup_brightness_fade_timeout(step_time);

EXIT:
	return;
}

/**
 * Blank display
 */
static void display_blank(void)
{
	cancel_brightness_fade_timeout();
	cached_brightness = 0;
	target_brightness = 0;
	mce_write_number_string_to_file(brightness_file, 0,
					&brightness_fp, TRUE, FALSE);
	backlight_ioctl(FB_BLANK_POWERDOWN);
}

/**
 * Dim display
 */
static void display_dim(void)
{
	/* If we unblank, switch on display immediately */
	if (cached_brightness == 0) {
		cached_brightness = dim_brightness;
		target_brightness = dim_brightness;
		backlight_ioctl(FB_BLANK_UNBLANK);
		mce_write_number_string_to_file(brightness_file,
						dim_brightness,
						&brightness_fp, TRUE, FALSE);
	} else {
		update_brightness_fade(dim_brightness);
	}
}

/**
 * Unblank display
 */
static void display_unblank(void)
{
	/* If we unblank, switch on display immediately */
	if (cached_brightness == 0) {
		cached_brightness = set_brightness;
		target_brightness = set_brightness;
		backlight_ioctl(FB_BLANK_UNBLANK);
		mce_write_number_string_to_file(brightness_file,
						set_brightness,
						&brightness_fp, TRUE, FALSE);
	} else {
		update_brightness_fade(set_brightness);
	}
}

/**
 * Display brightness trigger
 *
 * @note A brightness request is only sent if the value changed
 * @param data The display brightness stored in a pointer
 */
static void display_brightness_trigger(gconstpointer data)
{
	display_state_t display_state = datapipe_get_gint(display_state_pipe);
	gint new_brightness = GPOINTER_TO_INT(data);

	/* If the pipe is choked, ignore the value */
	if (new_brightness == 0)
		goto EXIT;

	/* Adjust the value, since it's a percentage value */
	new_brightness = (maximum_display_brightness * new_brightness) / 100;

	/* If we're just rehashing the same brightness value, don't bother */
	if ((new_brightness == cached_brightness) && (cached_brightness != -1))
		goto EXIT;

	/* The value we have here is for non-dimmed screen only */
	set_brightness = new_brightness;

	if ((display_state == MCE_DISPLAY_OFF) ||
	    (display_state == MCE_DISPLAY_DIM))
		goto EXIT;

	update_brightness_fade(new_brightness);

EXIT:
	return;
}

/**
 * Timeout callback for display blanking
 *
 * @param data Unused
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean blank_timeout_cb(gpointer data)
{
	(void)data;

	blank_timeout_cb_id = 0;

	(void)execute_datapipe(&display_state_pipe,
			       GINT_TO_POINTER(MCE_DISPLAY_OFF),
			       USE_INDATA, CACHE_INDATA);

	return FALSE;
}

/**
 * Cancel the display blanking timeout
 */
static void cancel_blank_timeout(void)
{
	/* Remove the timeout source for display blanking */
	if (blank_timeout_cb_id != 0) {
		g_source_remove(blank_timeout_cb_id);
		blank_timeout_cb_id = 0;
	}
}

/**
 * Setup blank timeout
 */
static void setup_blank_timeout(void)
{
	cancel_blank_timeout();
	cancel_dim_timeout();

	if (blanking_inhibited == TRUE)
		return;

	/* Setup new timeout */
	blank_timeout_cb_id =
		g_timeout_add_seconds(disp_blank_timeout,
				      blank_timeout_cb, NULL);
}

/**
 * Timeout callback for adaptive dimming timeout
 *
 * @param data Unused
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean adaptive_dimming_timeout_cb(gpointer data)
{
	(void)data;

	adaptive_dimming_timeout_cb_id = 0;
	adaptive_dimming_index = 0;

	return FALSE;
}

/**
 * Cancel the adaptive dimming timeout
 */
static void cancel_adaptive_dimming_timeout(void)
{
	/* Remove the timeout source for adaptive dimming */
	if (adaptive_dimming_timeout_cb_id != 0) {
		g_source_remove(adaptive_dimming_timeout_cb_id);
		adaptive_dimming_timeout_cb_id = 0;
	}
}

/**
 * Setup adaptive dimming timeout
 */
static void setup_adaptive_dimming_timeout(void)
{
	cancel_adaptive_dimming_timeout();

	if (adaptive_dimming_enabled == FALSE)
		goto EXIT;

	/* Setup new timeout */
	adaptive_dimming_timeout_cb_id =
		g_timeout_add_seconds(adaptive_dimming_threshold,
				      adaptive_dimming_timeout_cb, NULL);

EXIT:
	return;
}

/**
 * Timeout callback for display dimming
 *
 * @param data Unused
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean dim_timeout_cb(gpointer data)
{
	(void)data;

	dim_timeout_cb_id = 0;

	(void)execute_datapipe(&display_state_pipe,
			       GINT_TO_POINTER(MCE_DISPLAY_DIM),
			       USE_INDATA, CACHE_INDATA);

	return FALSE;
}

/**
 * Cancel display dimming timeout
 */
static void cancel_dim_timeout(void)
{
	/* Remove the timeout source for display dimming */
	if (dim_timeout_cb_id != 0) {
		g_source_remove(dim_timeout_cb_id);
		dim_timeout_cb_id = 0;
	}
}

/**
 * Setup dim timeout
 */
static void setup_dim_timeout(void)
{
	gint dim_timeout = disp_dim_timeout + bootup_dim_additional_timeout;

	cancel_blank_timeout();
	cancel_adaptive_dimming_timeout();
	cancel_dim_timeout();

	if (dimming_inhibited == TRUE)
		return;

	if (adaptive_dimming_enabled == TRUE) {
		gpointer *tmp = g_slist_nth_data(possible_dim_timeouts,
						 dim_timeout_index +
						 adaptive_dimming_index);

		if (tmp != NULL)
			dim_timeout = GPOINTER_TO_INT(tmp) +
				      bootup_dim_additional_timeout;
	}

	/* Setup new timeout */
	dim_timeout_cb_id =
		g_timeout_add_seconds(dim_timeout,
				      dim_timeout_cb, NULL);
}

/**
 * Timeout callback for display blanking pause
 *
 * @param data Unused
 * @return Always returns FALSE, to disable the timeout
 */
static gboolean blank_prevent_timeout_cb(gpointer data)
{
	(void)data;

	blank_prevent_timeout_cb_id = 0;

	update_blanking_inhibit(FALSE);

	return FALSE;
}

/**
 * Cancel blank prevention timeout
 */
static void cancel_blank_prevent(void)
{
	if (blank_prevent_timeout_cb_id != 0) {
		g_source_remove(blank_prevent_timeout_cb_id);
		blank_prevent_timeout_cb_id = 0;
	}
}

/**
 * Prevent screen blanking for display_timeout seconds
 */
static void request_display_blanking_pause(void)
{
	/* Also cancels any old timeouts */
	update_blanking_inhibit(TRUE);

	/* Setup new timeout */
	blank_prevent_timeout_cb_id =
		g_timeout_add_seconds(blank_prevent_timeout,
				      blank_prevent_timeout_cb, NULL);
}

/**
 * Enable/Disable blanking inhibit,
 * based on charger status and inhibit mode
 *
 * @param timed_inhibit TRUE for timed inhibiting,
 *                      FALSE for triggered inhibiting
 */
static void update_blanking_inhibit(gboolean timed_inhibit)
{
	display_state_t display_state = datapipe_get_gint(display_state_pipe);
	system_state_t system_state = datapipe_get_gint(system_state_pipe);
	alarm_ui_state_t alarm_ui_state =
				datapipe_get_gint(alarm_ui_state_pipe);
	call_state_t call_state = datapipe_get_gint(call_state_pipe);

	if ((system_state == MCE_STATE_ACTDEAD) &&
	    (charger_connected == TRUE) &&
	    ((alarm_ui_state == MCE_ALARM_UI_OFF_INT32) ||
	     (alarm_ui_state == MCE_ALARM_UI_INVALID_INT32))) {
		/* If there's no alarm UI visible and we're in acting dead,
		 * never inhibit blanking
		 */
		blanking_inhibited = FALSE;
		dimming_inhibited = FALSE;
		cancel_blank_prevent();
	} else if ((call_state == CALL_STATE_RINGING) ||
		   (blanking_inhibit_mode == INHIBIT_STAY_ON) ||
		   (blanking_inhibit_mode == INHIBIT_STAY_DIM) ||
		   (timed_inhibit == TRUE) ||
	           ((charger_connected == TRUE) &&
		    ((blanking_inhibit_mode == INHIBIT_STAY_ON_WITH_CHARGER) ||
		     (blanking_inhibit_mode ==
		      INHIBIT_STAY_DIM_WITH_CHARGER)))) {
		/* Always inhibit blanking */
		blanking_inhibited = TRUE;

		/* If the policy calls for it, also inhibit dimming;
		 * INHIBIT_STAY_ON{,WITH_CHARGER} doesn't affect the
		 * policy in acting dead though
		 */
		if ((((blanking_inhibit_mode == INHIBIT_STAY_ON_WITH_CHARGER) ||
		      (blanking_inhibit_mode == INHIBIT_STAY_ON)) &&
		     (system_state != MCE_STATE_ACTDEAD)) ||
		    (call_state == CALL_STATE_RINGING) ||
		    (timed_inhibit == TRUE)) {
			dimming_inhibited = TRUE;
		} else {
			dimming_inhibited = FALSE;
		}

		cancel_blank_prevent();
	} else if (blank_prevent_timeout_cb_id == 0) {
		blanking_inhibited = FALSE;
		dimming_inhibited = FALSE;
	}

	/* Reprogram timeouts, if necessary */
	if (display_state == MCE_DISPLAY_DIM)
		setup_blank_timeout();
	else if (display_state != MCE_DISPLAY_OFF)
		setup_dim_timeout();
}

/**
 * D-Bus reply handler for device lock inhibit
 *
 * @param pending_call The DBusPendingCall
 * @param data Unused
 */
static void devlock_inhibit_reply_dbus_cb(DBusPendingCall *pending_call,
					  void *data)
{
	DBusMessage *reply;
	dbus_int32_t retval;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	(void)data;

	mce_log(LL_DEBUG, "Received device lock inhibit reply");

	if ((reply = dbus_pending_call_steal_reply(pending_call)) == NULL) {
		mce_log(LL_ERR,
			"Device lock inhibit reply callback invoked, "
			"but no pending call available");
		goto EXIT;
	}

	/* Make sure we didn't get an error message */
	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
		char *error_msg;

		/* If we got an error, it's a string */
		if (dbus_message_get_args(reply, &error,
					  DBUS_TYPE_STRING, &error_msg,
					  DBUS_TYPE_INVALID) == FALSE) {
			mce_log(LL_CRIT,
				"Failed to get error reply argument "
				"from %s.%s: %s",
				DEVLOCK_SERVICE, DEVLOCK_SET,
				error.message);
			dbus_error_free(&error);
		} else {
			mce_log(LL_ERR,
				"D-Bus call to %s.%s failed: %s",
				DEVLOCK_SERVICE, DEVLOCK_SET,
				error_msg);
		}

		goto EXIT2;
	}

	/* Extract reply */
	if (dbus_message_get_args(reply, &error,
				  DBUS_TYPE_BOOLEAN, &retval,
				  DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_CRIT,
			"Failed to get reply argument from %s.%s: %s",
			DEVLOCK_SERVICE, DEVLOCK_SET,
			error.message);
		dbus_error_free(&error);
		goto EXIT2;
	}

	mce_log(LL_DEBUG,
		"Return value: %d",
		retval);

EXIT2:
	dbus_message_unref(reply);

EXIT:
	dbus_pending_call_unref(pending_call);

	return;
}

/**
 * Inhibit device lock
 */
static void inhibit_devicelock(void)
{
	dbus_int32_t lock_type = Device;
	dbus_int32_t lock_state = Inhibit;

	mce_log(LL_DEBUG,
		"Requesting device lock inhibit");

	dbus_send(DEVLOCK_SERVICE, DEVLOCK_PATH,
		  DEVLOCK_SERVICE, DEVLOCK_SET,
		  devlock_inhibit_reply_dbus_cb,
		  DBUS_TYPE_INT32, &lock_type,
		  DBUS_TYPE_INT32, &lock_state,
		  DBUS_TYPE_INVALID);
}

/**
 * Find the dim timeout index from a dim timeout
 *
 * @param dim_timeout The dim timeout to find the index for
 * @return The closest dim timeout index
 */
static guint find_dim_timeout_index(gint dim_timeout)
{
	gpointer tmp;
	guint i;

	for (i = 0;
	     ((tmp = g_slist_nth_data(possible_dim_timeouts, i)) != NULL) &&
	     GPOINTER_TO_INT(tmp) < dim_timeout; i++)
		/* Do nothing */;

	return i;
}

/**
 * GConf callback for display related settings
 *
 * @param gcc Unused
 * @param id Connection ID from gconf_client_notify_add()
 * @param entry The modified GConf entry
 * @param data Unused
 */
static void display_gconf_cb(GConfClient *const gcc, const guint id,
			     GConfEntry *const entry, gpointer const data)
{
	const GConfValue *gcv = gconf_entry_get_value(entry);

	(void)gcc;
	(void)data;

	/* Key is unset */
	if (gcv == NULL) {
		mce_log(LL_DEBUG,
			"GConf Key `%s' has been unset",
			gconf_entry_get_key(entry));
		goto EXIT;
	}

	if (id == disp_brightness_gconf_cb_id) {
		real_disp_brightness = gconf_value_get_int(gcv);

		if (psm_disp_brightness == -1) {
			(void)execute_datapipe(&display_brightness_pipe, GINT_TO_POINTER(real_disp_brightness), USE_INDATA, CACHE_INDATA);
		}
	} else if (id == disp_blank_timeout_gconf_cb_id) {
		disp_blank_timeout = gconf_value_get_int(gcv);

		/* Update blank prevent */
		update_blanking_inhibit(FALSE);

		/* Update inactivity timeout */
		(void)execute_datapipe(&inactivity_timeout_pipe,
				       GINT_TO_POINTER(disp_dim_timeout +
						       disp_blank_timeout),
				       USE_INDATA, CACHE_INDATA);
	} else if (id == adaptive_dimming_enabled_gconf_cb_id) {
		adaptive_dimming_enabled = gconf_value_get_bool(gcv);
		cancel_adaptive_dimming_timeout();
	} else if (id == adaptive_dimming_threshold_gconf_cb_id) {
		adaptive_dimming_threshold = gconf_value_get_int(gcv);
		cancel_adaptive_dimming_timeout();
	} else if (id == disp_dim_timeout_gconf_cb_id) {
		disp_dim_timeout = gconf_value_get_int(gcv);

		/* Find the closest match in the list of valid dim timeouts */
		dim_timeout_index = find_dim_timeout_index(disp_dim_timeout);
		adaptive_dimming_index = 0;

		/* Update blank prevent */
		update_blanking_inhibit(FALSE);

		/* Update inactivity timeout */
		(void)execute_datapipe(&inactivity_timeout_pipe,
				       GINT_TO_POINTER(disp_dim_timeout +
						       disp_blank_timeout),
				       USE_INDATA, CACHE_INDATA);
	} else if (id == blanking_inhibit_mode_gconf_cb_id) {
		blanking_inhibit_mode = gconf_value_get_int(gcv);

		/* Update blank prevent */
		update_blanking_inhibit(FALSE);
	} else {
		mce_log(LL_WARN,
			"Spurious GConf value received; confused!");
	}

EXIT:
	return;
}

/**
 * Send a display status reply or signal
 *
 * @param method_call A DBusMessage to reply to;
 *                    pass NULL to send a display status signal instead
 * @return TRUE on success, FALSE on failure
 */
static gboolean send_display_status(DBusMessage *const method_call)
{
	display_state_t display_state = datapipe_get_gint(display_state_pipe);
	DBusMessage *msg = NULL;
	const gchar *state = NULL;
	gboolean status = FALSE;

	switch (display_state) {
	case MCE_DISPLAY_OFF:
		state = MCE_DISPLAY_OFF_STRING;
		break;

	case MCE_DISPLAY_DIM:
		state = MCE_DISPLAY_DIM_STRING;
		break;

	case MCE_DISPLAY_ON:
	default:
		state = MCE_DISPLAY_ON_STRING;
		break;
	}

	mce_log(LL_DEBUG,
		"Sending display status: %s",
		state);

	/* If method_call is set, send a reply,
	 * otherwise, send a signal
	 */
	if (method_call != NULL) {
		msg = dbus_new_method_reply(method_call);
	} else {
		/* display_status_ind */
		msg = dbus_new_signal(MCE_SIGNAL_PATH, MCE_SIGNAL_IF,
				      MCE_DISPLAY_SIG);
	}

	/* Append the display status */
	if (dbus_message_append_args(msg,
				     DBUS_TYPE_STRING, &state,
				     DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_CRIT,
			"Failed to append %sargument to D-Bus message "
			"for %s.%s",
			method_call ? "reply " : "",
			method_call ? MCE_REQUEST_IF :
				      MCE_SIGNAL_IF,
			method_call ? MCE_DISPLAY_STATUS_GET :
				      MCE_DISPLAY_SIG);
		dbus_message_unref(msg);
		goto EXIT;
	}

	/* Send the message */
	status = dbus_send_message(msg);

EXIT:
	return status;
}

/**
 * D-Bus callback for the get display status method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean display_status_get_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;

	mce_log(LL_DEBUG,
		"Received display status get request");

	/* Try to send a reply that contains the current display status */
	if (send_display_status(msg) == FALSE)
		goto EXIT;

	status = TRUE;

EXIT:
	return status;
}

/**
 * Send a CABC status reply
 *
 * @param method_call A DBusMessage to reply to
 * @return TRUE on success, FALSE on failure
 */
static gboolean send_cabc_mode(DBusMessage *const method_call)
{
	const gchar *dbus_cabc_mode = NULL;
	DBusMessage *msg = NULL;
	gboolean status = FALSE;
	gint i;

	for (i = 0; cabc_mode_mapping[i].sysfs != NULL; i++) {
		if (!strcmp(cabc_mode_mapping[i].sysfs, cabc_mode)) {
			dbus_cabc_mode = cabc_mode_mapping[i].dbus;
			break;
		}
	}

	if (dbus_cabc_mode == NULL)
		dbus_cabc_mode = MCE_CABC_MODE_OFF;

	mce_log(LL_DEBUG,
		"Sending CABC mode: %s",
		dbus_cabc_mode);

	msg = dbus_new_method_reply(method_call);

	/* Append the CABC mode */
	if (dbus_message_append_args(msg,
				     DBUS_TYPE_STRING, &dbus_cabc_mode,
				     DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_CRIT,
			"Failed to append reply argument to D-Bus message "
			"for %s.%s",
			MCE_REQUEST_IF, MCE_CABC_MODE_GET);
		dbus_message_unref(msg);
		goto EXIT;
	}

	/* Send the message */
	status = dbus_send_message(msg);

EXIT:
	return status;
}

/**
 * D-Bus callback for the get CABC mode method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean cabc_mode_get_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;

	mce_log(LL_DEBUG,
		"Received CABC mode get request");

	/* Try to send a reply that contains the current CABC mode */
	if (send_cabc_mode(msg) == FALSE)
		goto EXIT;

	status = TRUE;

EXIT:
	return status;
}

/**
 * D-Bus callback for the display on method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean display_on_req_dbus_cb(DBusMessage *const msg)
{
	call_state_t call_state = datapipe_get_gint(call_state_pipe);
	dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
	gboolean status = FALSE;

	mce_log(LL_DEBUG,
		"Received display on request");

	if (call_state != CALL_STATE_RINGING) {
		(void)execute_datapipe(&display_state_pipe,
				       GINT_TO_POINTER(MCE_DISPLAY_ON),
				       USE_INDATA, CACHE_INDATA);
	}

	if (no_reply == FALSE) {
		DBusMessage *reply = dbus_new_method_reply(msg);

		status = dbus_send_message(reply);
	} else {
		status = TRUE;
	}

	return status;
}

/**
 * D-Bus callback for the display dim method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean display_dim_req_dbus_cb(DBusMessage *const msg)
{
	dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
	gboolean status = FALSE;

	mce_log(LL_DEBUG,
		"Received display dim request");

	(void)execute_datapipe(&display_state_pipe,
			       GINT_TO_POINTER(MCE_DISPLAY_DIM),
			       USE_INDATA, CACHE_INDATA);

	if (no_reply == FALSE) {
		DBusMessage *reply = dbus_new_method_reply(msg);

		status = dbus_send_message(reply);
	} else {
		status = TRUE;
	}

	return status;
}

/**
 * D-Bus callback for the display low power mode method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean display_low_power_mode_req_dbus_cb(DBusMessage *const msg)
{
	dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
	gboolean status = FALSE;

	mce_log(LL_DEBUG,
		"Received display low power mode request");

	if (display_low_power_mode_supported == TRUE) {
		(void)execute_datapipe(&display_state_pipe,
				       GINT_TO_POINTER(MCE_DISPLAY_LOW_POWER),
				       USE_INDATA, CACHE_INDATA);
	} else {
		(void)execute_datapipe(&display_state_pipe,
				       GINT_TO_POINTER(MCE_DISPLAY_OFF),
				       USE_INDATA, CACHE_INDATA);
		mce_log(LL_DEBUG,
			"Display low power mode not supported; "
			"using display off instead");
	}

	if (no_reply == FALSE) {
		DBusMessage *reply = dbus_new_method_reply(msg);

		status = dbus_send_message(reply);
	} else {
		status = TRUE;
	}

	return status;
}

/**
 * D-Bus callback for the display off method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean display_off_req_dbus_cb(DBusMessage *const msg)
{
	dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
	gboolean status = FALSE;

	mce_log(LL_DEBUG,
		"Received display off request");

	(void)execute_datapipe(&display_state_pipe,
			       GINT_TO_POINTER(MCE_DISPLAY_OFF),
			       USE_INDATA, CACHE_INDATA);

	if (no_reply == FALSE) {
		DBusMessage *reply = dbus_new_method_reply(msg);

		status = dbus_send_message(reply);
	} else {
		status = TRUE;
	}

	return status;
}

/**
 * Remove a blanking pause with its D-Bus monitor
 *
 * @param name The name of the D-Bus owner to remove
 * @return TRUE on success, FALSE if name is NULL
 */
static gboolean remove_blanking_pause(const gchar *name)
{
	gboolean status = FALSE;
	gssize count;

	if (name == NULL)
		goto EXIT;

	/* Remove the name monitor for the blanking pause requester;
	 * if we don't have any requesters left, remove the timeout
	 */
	count = mce_dbus_owner_monitor_remove(name,
					      &blanking_pause_monitor_list);

	if (count == 0) {
		cancel_blank_prevent();
		update_blanking_inhibit(FALSE);
	}

	status = TRUE;

EXIT:
	return status;
}

/**
 * D-Bus callback used for monitoring the process that requested
 * blanking prevention; if that process exits, immediately
 * cancel the blanking timeout and resume normal operation
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean blanking_pause_owner_monitor_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;
	const gchar *old_name;
	const gchar *new_name;
	const gchar *service;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	/* Extract result */
	if (dbus_message_get_args(msg, &error,
				  DBUS_TYPE_STRING, &service,
				  DBUS_TYPE_STRING, &old_name,
				  DBUS_TYPE_STRING, &new_name,
				  DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_ERR,
			"Failed to get argument from %s.%s; %s",
			"org.freedesktop.DBus", "NameOwnerChanged",
			error.message);
		dbus_error_free(&error);
		goto EXIT;
	}

	remove_blanking_pause(old_name);
	status = TRUE;

EXIT:
	return status;
}

/**
 * D-Bus callback for display cancel blanking prevent request method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean display_cancel_blanking_pause_req_dbus_cb(DBusMessage *const msg)
{
	dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
	const gchar *sender = dbus_message_get_sender(msg);
	gboolean status = FALSE;

	mce_log(LL_DEBUG,
		"Received cancel blanking pause request from %s",
		(sender == NULL) ? "(unknown)" : sender);

	remove_blanking_pause(sender);

	if (no_reply == FALSE) {
		DBusMessage *reply = dbus_new_method_reply(msg);

		status = dbus_send_message(reply);
	} else {
		status = TRUE;
	}

	return status;
}

/**
 * D-Bus callback for display blanking prevent request method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean display_blanking_pause_req_dbus_cb(DBusMessage *const msg)
{
	dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
	const gchar *sender = dbus_message_get_sender(msg);
	gboolean status = FALSE;

	mce_log(LL_DEBUG,
		"Received blanking pause request from %s",
		(sender == NULL) ? "(unknown)" : sender);

	request_display_blanking_pause();
	inhibit_devicelock();

	if (mce_dbus_owner_monitor_add(sender,
				       blanking_pause_owner_monitor_dbus_cb,
				       &blanking_pause_monitor_list,
				       MAX_MONITORED_SERVICES) == -1) {
		mce_log(LL_INFO,
			"Failed to add name owner monitoring for `%s'",
			sender);
	}

	if (no_reply == FALSE) {
		DBusMessage *reply = dbus_new_method_reply(msg);

		status = dbus_send_message(reply);
	} else {
		status = TRUE;
	}

	return status;
}

/**
 * D-Bus callback used for monitoring the process that requested
 * CABC mode change; if that process exits, immediately
 * restore the CABC mode to the default
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean cabc_mode_owner_monitor_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;
	const gchar *old_name;
	const gchar *new_name;
	const gchar *service;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	/* Extract result */
	if (dbus_message_get_args(msg, &error,
				  DBUS_TYPE_STRING, &service,
				  DBUS_TYPE_STRING, &old_name,
				  DBUS_TYPE_STRING, &new_name,
				  DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_ERR,
			"Failed to get argument from %s.%s; %s",
			"org.freedesktop.DBus", "NameOwnerChanged",
			error.message);
		dbus_error_free(&error);
		goto EXIT;
	}

	/* Remove the name monitor for the CABC mode */
	mce_dbus_owner_monitor_remove_all(&cabc_mode_monitor_list);
	set_cabc_mode(DEFAULT_CABC_MODE);

	status = TRUE;

EXIT:
	return status;
}

/**
 * D-Bus callback for the set CABC mode method call
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean cabc_mode_req_dbus_cb(DBusMessage *const msg)
{
	dbus_bool_t no_reply = dbus_message_get_no_reply(msg);
	const gchar *sender = dbus_message_get_sender(msg);
	const gchar *sysfs_cabc_mode = NULL;
	const gchar *dbus_cabc_mode = NULL;
	gboolean status = FALSE;
	gint i;
	DBusError error;

	/* Register error channel */
	dbus_error_init(&error);

	mce_log(LL_DEBUG,
		"Received set CABC mode request from %s",
		(sender == NULL) ? "(unknown)" : sender);

	/* Extract result */
	if (dbus_message_get_args(msg, &error,
				  DBUS_TYPE_STRING, &dbus_cabc_mode,
				  DBUS_TYPE_INVALID) == FALSE) {
		mce_log(LL_ERR,
			"Failed to get argument from %s.%s; %s",
			MCE_REQUEST_IF, MCE_CABC_MODE_REQ,
			error.message);
		dbus_error_free(&error);
		goto EXIT;
	}

	for (i = 0; cabc_mode_mapping[i].dbus != NULL; i++) {
		if (!strcmp(cabc_mode_mapping[i].dbus, dbus_cabc_mode)) {
			sysfs_cabc_mode = cabc_mode_mapping[i].sysfs;
		}
	}

	/* Use the default if the requested mode was invalid */
	if (sysfs_cabc_mode == NULL) {
		mce_log(LL_WARN,
			"Invalid CABC mode requested; using %s",
			DEFAULT_CABC_MODE);
		sysfs_cabc_mode = DEFAULT_CABC_MODE;
	}

	set_cabc_mode(sysfs_cabc_mode);

	/* We only ever monitor one owner; latest wins */
	mce_dbus_owner_monitor_remove_all(&cabc_mode_monitor_list);

	if (mce_dbus_owner_monitor_add(sender,
				       cabc_mode_owner_monitor_dbus_cb,
				       &cabc_mode_monitor_list,
				       1) == -1) {
		mce_log(LL_INFO,
			"Failed to add name owner monitoring for `%s'",
			sender);
	}

	/* If reply is wanted, send the current CABC mode */
	if (no_reply == FALSE) {
		DBusMessage *reply = dbus_new_method_reply(msg);

		for (i = 0; cabc_mode_mapping[i].sysfs != NULL; i++) {
			if (!strcmp(sysfs_cabc_mode, cabc_mode_mapping[i].sysfs)) {
				dbus_message_append_args(reply, DBUS_TYPE_STRING, &cabc_mode_mapping[i].dbus, DBUS_TYPE_INVALID);
				break;
			}
		}
		status = dbus_send_message(reply);
	} else {
		status = TRUE;
	}

EXIT:
	return status;
}

/**
 * D-Bus callback for the desktop startup notification signal
 *
 * @param msg The D-Bus message
 * @return TRUE on success, FALSE on failure
 */
static gboolean desktop_startup_dbus_cb(DBusMessage *const msg)
{
	gboolean status = FALSE;

	(void)msg;

	mce_log(LL_DEBUG,
		"Received desktop startup notification");

	execute_datapipe_output_triggers(&led_pattern_deactivate_pipe,
					 MCE_LED_PATTERN_POWER_ON, USE_INDATA);

	mce_rem_submode_int32(MCE_BOOTUP_SUBMODE);

	/* Restore normal inactivity timeout */
	(void)execute_datapipe(&inactivity_timeout_pipe,
			       GINT_TO_POINTER(disp_dim_timeout +
					       disp_blank_timeout),
			       USE_INDATA, CACHE_INDATA);

	/* Remove the additional timeout */
	bootup_dim_additional_timeout = 0;

	/* Update blank prevent */
	update_blanking_inhibit(FALSE);

	status = TRUE;

	return status;
}

/**
 * Handle display state change
 *
 * @param data The display state stored in a pointer
 */
static void display_state_trigger(gconstpointer data)
{
	/** Cached display state */
	static display_state_t cached_display_state = MCE_DISPLAY_UNDEF;
	display_state_t display_state = GPOINTER_TO_INT(data);
	submode_t submode = mce_get_submode_int32();

	switch (display_state) {
	case MCE_DISPLAY_OFF:
		cancel_dim_timeout();
		cancel_adaptive_dimming_timeout();
		adaptive_dimming_index = 0;
		cancel_blank_timeout();
		break;

	case MCE_DISPLAY_DIM:
		cancel_dim_timeout();
		setup_adaptive_dimming_timeout();
		setup_blank_timeout();
		break;

	case MCE_DISPLAY_ON:
	default:
		/* The tklock has its own timeout */
		if ((submode & MCE_TKLOCK_SUBMODE) == 0) {
			setup_dim_timeout();
		}

		break;
	}

	/* If we already have the right state,
	 * we're done here
	 */
	if (cached_display_state == display_state)
		goto EXIT;

	switch (display_state) {
	case MCE_DISPLAY_OFF:
		display_blank();
		break;

	case MCE_DISPLAY_DIM:
		display_dim();
		break;

	case MCE_DISPLAY_ON:
	default:
		display_unblank();
		break;
	}

	/* This will send the correct state
	 * since the pipe contains the new value
	 */
	send_display_status(NULL);

	/* Update the cached value */
	cached_display_state = display_state;

EXIT:
	return;
}

/**
 * Handle submode change
 *
 * @param data The submode stored in a pointer
 */
static void submode_trigger(gconstpointer data)
{
	static submode_t old_submode = MCE_NORMAL_SUBMODE;
	submode_t submode = GPOINTER_TO_INT(data);

	/* Avoid unnecessary updates:
	 * Note: this *must* be binary or/and,
	 *       not logical, else it won't work,
	 *       for (hopefully) obvious reasons
	 */
	if ((old_submode | submode) & MCE_TRANSITION_SUBMODE)
		update_blanking_inhibit(FALSE);

	submode = old_submode;
}

/**
 * Datapipe trigger for the charger state
 *
 * @param data TRUE if the charger was connected,
 *	       FALSE if the charger was disconnected
 */
static void charger_state_trigger(gconstpointer data)
{
	charger_connected = GPOINTER_TO_INT(data);

	update_blanking_inhibit(FALSE);
}

/**
 * Datapipe trigger for device inactivity
 *
 * @param data The inactivity stored in a pointer;
 *             TRUE if the device is inactive,
 *             FALSE if the device is active
 */
static void device_inactive_trigger(gconstpointer data)
{
	system_state_t system_state = datapipe_get_gint(system_state_pipe);
	alarm_ui_state_t alarm_ui_state =
				datapipe_get_gint(alarm_ui_state_pipe);
	gboolean device_inactive = GPOINTER_TO_INT(data);
	submode_t submode = mce_get_submode_int32();

	/* Unblank screen on device activity,
	 * unless the device is in acting dead and no alarm is visible
	 * or if the tklock is active
	 */
	if (((system_state == MCE_STATE_USER) ||
	     ((system_state == MCE_STATE_ACTDEAD) &&
	      ((alarm_ui_state == MCE_ALARM_UI_VISIBLE_INT32) ||
	       (alarm_ui_state == MCE_ALARM_UI_RINGING_INT32)))) &&
	    (device_inactive == FALSE) &&
	    ((submode & MCE_TKLOCK_SUBMODE) == 0)) {
		/* Adjust the adaptive dimming timeouts,
		 * even if we don't use them
		 */
		if (adaptive_dimming_timeout_cb_id != 0) {
			if (g_slist_nth(possible_dim_timeouts,
					dim_timeout_index +
					adaptive_dimming_index + 1) != NULL)
				adaptive_dimming_index++;
		}

		(void)execute_datapipe(&display_state_pipe,
				       GINT_TO_POINTER(MCE_DISPLAY_ON),
				       USE_INDATA, CACHE_INDATA);
	}
}

/**
 * Datapipe trigger for call state
 *
 * @param data Unused
 */
static void call_state_trigger(gconstpointer data)
{
	(void)data;

	update_blanking_inhibit(FALSE);
}

/**
 * Datapipe trigger for the power saving mode
 *
 * @param data Unused
 */
static void power_saving_mode_trigger(gconstpointer data)
{
	gboolean power_saving_mode = GPOINTER_TO_INT(data);

	if (power_saving_mode == TRUE) {
		/* Override the CABC mode and brightness setting */
		psm_cabc_mode = DEFAULT_PSM_CABC_MODE;
		psm_disp_brightness = DEFAULT_PSM_DISP_BRIGHTNESS;

		(void)execute_datapipe(&display_brightness_pipe, GINT_TO_POINTER(psm_disp_brightness), USE_INDATA, CACHE_INDATA);
		set_cabc_mode(psm_cabc_mode);
	} else {
		/* Restore the CABC mode and brightness setting */
		psm_cabc_mode = NULL;
		psm_disp_brightness = -1;

		(void)execute_datapipe(&display_brightness_pipe, GINT_TO_POINTER(real_disp_brightness), USE_INDATA, CACHE_INDATA);
		set_cabc_mode(cabc_mode);
	}
}

/**
 * Handle system state change
 *
 * @param data The system state stored in a pointer
 */
static void system_state_trigger(gconstpointer data)
{
	alarm_ui_state_t alarm_ui_state =
				datapipe_get_gint(alarm_ui_state_pipe);
	system_state_t system_state = GPOINTER_TO_INT(data);

	switch (system_state) {
	case MCE_STATE_USER:
		(void)execute_datapipe(&display_state_pipe,
				       GINT_TO_POINTER(MCE_DISPLAY_ON),
				       USE_INDATA, CACHE_INDATA);
		break;

	case MCE_STATE_ACTDEAD:
		if ((alarm_ui_state == MCE_ALARM_UI_RINGING_INT32) ||
		    (alarm_ui_state == MCE_ALARM_UI_VISIBLE_INT32)) {
			(void)execute_datapipe(&display_state_pipe,
					       GINT_TO_POINTER(MCE_DISPLAY_ON),
					       USE_INDATA, CACHE_INDATA);
		}

		break;

	case MCE_STATE_SHUTDOWN:
	case MCE_STATE_REBOOT:
	case MCE_STATE_UNDEF:
	default:
		break;
	}

	return;
}

/**
 * Init function for the display handling module
 *
 * @todo XXX status needs to be set on error!
 *
 * @param module Unused
 * @return NULL on success, a string with an error message on failure
 */
G_MODULE_EXPORT const gchar *g_module_check_init(GModule *module);
const gchar *g_module_check_init(GModule *module)
{
	submode_t submode = mce_get_submode_int32();
	gchar *str = NULL;
	gulong tmp;

	(void)module;

	/* Initialise the display type and the relevant paths */
	(void)get_display_type();

	if ((submode & MCE_TRANSITION_SUBMODE) != 0) {
		mce_add_submode_int32(MCE_BOOTUP_SUBMODE);
		bootup_dim_additional_timeout = BOOTUP_DIM_ADDITIONAL_TIMEOUT;
	} else {
		bootup_dim_additional_timeout = 0;
	}

	/* Append triggers/filters to datapipes */
	append_output_trigger_to_datapipe(&system_state_pipe,
					  system_state_trigger);
	append_output_trigger_to_datapipe(&charger_state_pipe,
					  charger_state_trigger);
	append_output_trigger_to_datapipe(&display_brightness_pipe,
					  display_brightness_trigger);
	append_output_trigger_to_datapipe(&display_state_pipe,
					  display_state_trigger);
	append_output_trigger_to_datapipe(&submode_pipe,
					  submode_trigger);
	append_output_trigger_to_datapipe(&device_inactive_pipe,
					  device_inactive_trigger);
	append_output_trigger_to_datapipe(&call_state_pipe,
					  call_state_trigger);
	append_output_trigger_to_datapipe(&power_saving_mode_pipe,
					  power_saving_mode_trigger);

	/* Get maximum brightness */
	if (mce_read_number_string_from_file(max_brightness_file,
					     &tmp, NULL, FALSE,
					     TRUE) == FALSE) {
		mce_log(LL_ERR,
			"Could not read the maximum brightness from %s; "
			"defaulting to %d",
			max_brightness_file,
			DEFAULT_MAXIMUM_DISPLAY_BRIGHTNESS);
		tmp = DEFAULT_MAXIMUM_DISPLAY_BRIGHTNESS;
	}

	maximum_display_brightness = tmp;
	dim_brightness = (maximum_display_brightness *
			  DEFAULT_DIM_BRIGHTNESS) / 100;

	set_cabc_mode(DEFAULT_CABC_MODE);

	/* Display brightness */
	/* Since we've set a default, error handling is unnecessary */
	(void)mce_gconf_get_int(MCE_GCONF_DISPLAY_BRIGHTNESS_PATH,
				&real_disp_brightness);

	/* Use the current brightness as cached brightness on startup,
	 * and fade from that value
	 */
	if (mce_read_number_string_from_file(brightness_file,
					     &tmp, NULL, FALSE,
					     TRUE) == FALSE) {
		mce_log(LL_ERR,
			"Could not read the current brightness from %s",
			brightness_file);
		cached_brightness = -1;
	} else {
		cached_brightness = tmp;
	}

	(void)execute_datapipe(&display_brightness_pipe,
			       GINT_TO_POINTER(real_disp_brightness),
			       USE_INDATA, CACHE_INDATA);

	if (mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
				   MCE_GCONF_DISPLAY_BRIGHTNESS_PATH,
				   display_gconf_cb,
				   &disp_brightness_gconf_cb_id) == FALSE)
		goto EXIT;

	/* Display blank */
	/* Since we've set a default, error handling is unnecessary */
	(void)mce_gconf_get_int(MCE_GCONF_DISPLAY_BLANK_TIMEOUT_PATH,
				&disp_blank_timeout);

	if (mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
				   MCE_GCONF_DISPLAY_BLANK_TIMEOUT_PATH,
				   display_gconf_cb,
				   &disp_blank_timeout_gconf_cb_id) == FALSE)
		goto EXIT;

	/* Use adaptive display dim timeout */
	/* Since we've set a default, error handling is unnecessary */
	(void)mce_gconf_get_bool(MCE_GCONF_DISPLAY_ADAPTIVE_DIMMING_PATH,
				 &adaptive_dimming_enabled);

	if (mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
				   MCE_GCONF_DISPLAY_ADAPTIVE_DIMMING_PATH,
				   display_gconf_cb,
				   &adaptive_dimming_enabled_gconf_cb_id) == FALSE)
		goto EXIT;

	/* Possible dim timeouts */
	if (mce_gconf_get_int_list(MCE_GCONF_DISPLAY_DIM_TIMEOUT_LIST_PATH,
				   &possible_dim_timeouts) == FALSE)
		goto EXIT;

	/* Adaptive display dimming threshold */
	/* Since we've set a default, error handling is unnecessary */
	(void)mce_gconf_get_int(MCE_GCONF_DISPLAY_ADAPTIVE_DIM_THRESHOLD_PATH,
				&adaptive_dimming_threshold);

	if (mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
				   MCE_GCONF_DISPLAY_ADAPTIVE_DIM_THRESHOLD_PATH,
				   display_gconf_cb,
				   &adaptive_dimming_threshold_gconf_cb_id) == FALSE)
		goto EXIT;

	/* Display dim */
	/* Since we've set a default, error handling is unnecessary */
	(void)mce_gconf_get_int(MCE_GCONF_DISPLAY_DIM_TIMEOUT_PATH,
				&disp_dim_timeout);

	dim_timeout_index = find_dim_timeout_index(disp_dim_timeout);
	adaptive_dimming_index = 0;

	if (mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
				   MCE_GCONF_DISPLAY_DIM_TIMEOUT_PATH,
				   display_gconf_cb,
				   &disp_dim_timeout_gconf_cb_id) == FALSE)
		goto EXIT;

	/* Update inactivity timeout */
	(void)execute_datapipe(&inactivity_timeout_pipe,
			       GINT_TO_POINTER(disp_dim_timeout +
					       disp_blank_timeout +
					       bootup_dim_additional_timeout),
			       USE_INDATA, CACHE_INDATA);

	/* Don't blank on charger */
	/* Since we've set a default, error handling is unnecessary */
	(void)mce_gconf_get_int(MCE_GCONF_BLANKING_INHIBIT_MODE_PATH,
				&blanking_inhibit_mode);

	if (mce_gconf_notifier_add(MCE_GCONF_DISPLAY_PATH,
				   MCE_GCONF_BLANKING_INHIBIT_MODE_PATH,
				   display_gconf_cb,
				   &blanking_inhibit_mode_gconf_cb_id) == FALSE)
		goto EXIT;

	/* get_display_status */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_DISPLAY_STATUS_GET,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 display_status_get_dbus_cb) == NULL)
		goto EXIT;

	/* get_cabc_mode */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_CABC_MODE_GET,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 cabc_mode_get_dbus_cb) == NULL)
		goto EXIT;

	/* req_display_state_on */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_DISPLAY_ON_REQ,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 display_on_req_dbus_cb) == NULL)
		goto EXIT;

	/* req_display_state_dim */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_DISPLAY_DIM_REQ,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 display_dim_req_dbus_cb) == NULL)
		goto EXIT;

	/* req_display_state_low_power */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_DISPLAY_LOW_POWER_MODE_REQ,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 display_low_power_mode_req_dbus_cb) == NULL)
		goto EXIT;

	/* req_display_state_off */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_DISPLAY_OFF_REQ,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 display_off_req_dbus_cb) == NULL)
		goto EXIT;

	/* req_display_blanking_pause */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_PREVENT_BLANK_REQ,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 display_blanking_pause_req_dbus_cb) == NULL)
		goto EXIT;

	/* req_display_cancel_blanking_pause */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_CANCEL_PREVENT_BLANK_REQ,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 display_cancel_blanking_pause_req_dbus_cb) == NULL)
		goto EXIT;

	/* req_cabc_mode */
	if (mce_dbus_handler_add(MCE_REQUEST_IF,
				 MCE_CABC_MODE_REQ,
				 NULL,
				 DBUS_MESSAGE_TYPE_METHOD_CALL,
				 cabc_mode_req_dbus_cb) == NULL)
		goto EXIT;

	/* Desktop readiness signal */
	if (mce_dbus_handler_add("com.nokia.startup.signal",
				 "desktop_visible",
				 NULL,
				 DBUS_MESSAGE_TYPE_SIGNAL,
				 desktop_startup_dbus_cb) == NULL)
		goto EXIT;

	/* Get configuration options */
	str = mce_conf_get_string(MCE_CONF_DISPLAY_GROUP,
				  MCE_CONF_BRIGHTNESS_INCREASE_POLICY,
				  "",
				  NULL);

	brightness_increase_policy = mce_translate_string_to_int_with_default(brightness_change_policy_translation, str, DEFAULT_BRIGHTNESS_INCREASE_POLICY);
	g_free(str);

	str = mce_conf_get_string(MCE_CONF_DISPLAY_GROUP,
				  MCE_CONF_BRIGHTNESS_DECREASE_POLICY,
				  "",
				  NULL);

	brightness_decrease_policy = mce_translate_string_to_int_with_default(brightness_change_policy_translation, str, DEFAULT_BRIGHTNESS_DECREASE_POLICY);
	g_free(str);

	brightness_increase_step_time =
		mce_conf_get_int(MCE_CONF_DISPLAY_GROUP,
				 MCE_CONF_STEP_TIME_INCREASE,
				 DEFAULT_BRIGHTNESS_INCREASE_STEP_TIME,
				 NULL);

	brightness_decrease_step_time =
		mce_conf_get_int(MCE_CONF_DISPLAY_GROUP,
				 MCE_CONF_STEP_TIME_DECREASE,
				 DEFAULT_BRIGHTNESS_DECREASE_STEP_TIME,
				 NULL);

	brightness_increase_constant_time =
		mce_conf_get_int(MCE_CONF_DISPLAY_GROUP,
				 MCE_CONF_CONSTANT_TIME_INCREASE,
				 DEFAULT_BRIGHTNESS_INCREASE_CONSTANT_TIME,
				 NULL);

	brightness_decrease_constant_time =
		mce_conf_get_int(MCE_CONF_DISPLAY_GROUP,
				 MCE_CONF_CONSTANT_TIME_DECREASE,
				 DEFAULT_BRIGHTNESS_DECREASE_CONSTANT_TIME,
				 NULL);

	/* Request display on to get the state machine in sync */
	(void)execute_datapipe(&display_state_pipe,
			       GINT_TO_POINTER(MCE_DISPLAY_ON),
			       USE_INDATA, CACHE_INDATA);

EXIT:
	return NULL;
}

/**
 * Exit function for the display handling module
 *
 * @todo D-Bus unregistration
 *
 * @param module Unused
 */
G_MODULE_EXPORT void g_module_unload(GModule *module);
void g_module_unload(GModule *module)
{
	(void)module;

	/* Remove triggers/filters from datapipes */
	remove_output_trigger_from_datapipe(&power_saving_mode_pipe,
					    power_saving_mode_trigger);
	remove_output_trigger_from_datapipe(&call_state_pipe,
					    call_state_trigger);
	remove_output_trigger_from_datapipe(&device_inactive_pipe,
					    device_inactive_trigger);
	remove_output_trigger_from_datapipe(&submode_pipe,
					    submode_trigger);
	remove_output_trigger_from_datapipe(&display_state_pipe,
					    display_state_trigger);
	remove_output_trigger_from_datapipe(&display_brightness_pipe,
					    display_brightness_trigger);
	remove_output_trigger_from_datapipe(&charger_state_pipe,
					    charger_state_trigger);
	remove_output_trigger_from_datapipe(&system_state_pipe,
					    system_state_trigger);

	/* Free lists */
	g_slist_free(possible_dim_timeouts);

	/* Free strings */
	g_free(brightness_file);
	g_free(max_brightness_file);
	g_free(cabc_mode_file);
	g_free(cabc_available_modes_file);

	/* Remove all timer sources */
	cancel_blank_prevent();
	cancel_brightness_fade_timeout();
	cancel_dim_timeout();
	cancel_adaptive_dimming_timeout();
	cancel_blank_timeout();

	return;
}
