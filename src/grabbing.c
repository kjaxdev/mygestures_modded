/*

 Copyright 2008-2016 Lucas Augusto Deters
 Copyright 2005 Nir Tzachar

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2, or (at your option)
 any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 one line to give the program's name and an idea of what it does.
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#include <X11/extensions/XTest.h>	/* emulating device events */
#include <X11/extensions/XInput2.h> /* capturing device events */

#include "drawing/drawing-brush-image.h"

#include "grabbing.h"
#include "grabbing-synaptics.h"
#include "actions.h"

#ifndef MAX_STROKES_PER_CAPTURE
#define MAX_STROKES_PER_CAPTURE 63 /*TODO*/
#endif

const char stroke_representations[] = {' ', 'L', 'R', 'U', 'D', '1', '3', '7',
									   '9'};

static void grabber_open_display(Grabber *self)
{

	self->dpy = XOpenDisplay(NULL);

	if (!XQueryExtension(self->dpy, "XInputExtension", &(self->opcode),
						 &(self->event), &(self->error)))
	{
		printf("X Input extension not available.\n");
		exit(-1);
	}

	int major = 2, minor = 0;
	if (XIQueryVersion(self->dpy, &major, &minor) == BadRequest)
	{
		printf("XI2 not available. Server supports %d.%d\n", major, minor);
		exit(-1);
	}
}

static struct brush_image_t *get_brush_image(char *color)
{

	struct brush_image_t *brush_image = NULL;

	if (!color)
		brush_image = NULL;
	else if (strcasecmp(color, "red") == 0)
		brush_image = &brush_image_red;
	else if (strcasecmp(color, "green") == 0)
		brush_image = &brush_image_green;
	else if (strcasecmp(color, "yellow") == 0)
		brush_image = &brush_image_yellow;
	else if (strcasecmp(color, "white") == 0)
		brush_image = &brush_image_white;
	else if (strcasecmp(color, "purple") == 0)
		brush_image = &brush_image_purple;
	else if (strcasecmp(color, "blue") == 0)
		brush_image = &brush_image_blue;
	else
		brush_image = NULL;

	return brush_image;
}

static void grabber_init_drawing(Grabber *self)
{

	assert(self->dpy);

	int err = 0;
	int scr = DefaultScreen(self->dpy);

	if (self->brush_image)
	{

		err = backing_init(&(self->backing), self->dpy,
						   DefaultRootWindow(self->dpy), DisplayWidth(self->dpy, scr),
						   DisplayHeight(self->dpy, scr), DefaultDepth(self->dpy, scr));
		if (err)
		{
			fprintf(stderr, "cannot open backing store.... \n");
		}
		err = brush_init(&(self->brush), &(self->backing), self->brush_image);
		if (err)
		{
			fprintf(stderr, "cannot init brush.... \n");
		}
	}
}

static Status fetch_window_title(Display *dpy, Window w, char **out_window_title)
{
    XTextProperty text_prop;
    char **list = NULL;
    int num = 0;
    Atom net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);

    /* Try _NET_WM_NAME (UTF8) first */
    if (XGetTextProperty(dpy, w, &text_prop, net_wm_name) && text_prop.value) {
        if (Xutf8TextPropertyToTextList(dpy, &text_prop, &list, &num) >= Success && num > 0 && list && *list) {
            *out_window_title = strdup(*list);
            XFree(text_prop.value);
            XFreeStringList(list);
            return 1;
        }
        XFree(text_prop.value);
    }

    /* Fallback to WM_NAME */
    if (XGetWMName(dpy, w, &text_prop) && text_prop.value) {
        if (Xutf8TextPropertyToTextList(dpy, &text_prop, &list, &num) >= Success && num > 0 && list && *list) {
            *out_window_title = strdup(*list);
            XFree(text_prop.value);
            XFreeStringList(list);
            return 1;
        }
        XFree(text_prop.value);
    }

    /* nothing found */
    *out_window_title = strdup("");
    return 1;
}

/*
 * Return a window_info struct for the focused window at a given Display.
 */
static ActiveWindowInfo *get_active_window_info(Display *dpy, Window win)
{
    int ret, val;

    char *win_title = NULL;
    ret = fetch_window_title(dpy, win, &win_title);

    ActiveWindowInfo *ans = malloc(sizeof(ActiveWindowInfo));
    bzero(ans, sizeof(ActiveWindowInfo));

    char *win_class = NULL;
    XClassHint class_hints;

    /* Try class on this window */
    if (XGetClassHint(dpy, win, &class_hints)) {
        if (class_hints.res_class)
            win_class = class_hints.res_class;
    }

    /* If title or class are empty, try the children (common when window is reparented) */
    if ((!win_title || win_title[0] == '\0') || (!win_class || win_class[0] == '\0')) {
        Window root_return, parent_return, *children = NULL;
        unsigned int nchildren = 0;
        if (XQueryTree(dpy, win, &root_return, &parent_return, &children, &nchildren)) {
            for (unsigned int i = 0; i < nchildren; ++i) {
                /* try to get a better title */
                if ((!win_title || win_title[0] == '\0')) {
                    char *child_title = NULL;
                    if (fetch_window_title(dpy, children[i], &child_title) && child_title && child_title[0] != '\0') {
                        free(win_title);
                        win_title = child_title;
                    } else {
                        free(child_title);
                    }
                }
                /* try to get a better class */
                if ((!win_class || win_class[0] == '\0')) {
                    XClassHint ch;
                    if (XGetClassHint(dpy, children[i], &ch)) {
                        if (ch.res_class) {
                            if (win_class && win_class[0] != '\0') {
                                /* keep existing */
                                XFree(ch.res_class);
                                XFree(ch.res_name);
                            } else {
                                win_class = ch.res_class;
                                XFree(ch.res_name);
                            }
                        }
                    }
                }
                if ((win_title && win_title[0] != '\0') && (win_class && win_class[0] != '\0'))
                    break;
            }
            if (children)
                XFree(children);
        }
    }

    /* Normalize to non-NULL strings */
    if (win_class) {
        ans->class = win_class;
    } else {
        ans->class = strdup("");
    }

    if (win_title) {
        ans->title = win_title;
    } else {
        ans->title = strdup("");
    }

    return ans;
}

static Window get_parent_window(Display *dpy, Window w)
{
    Window root_return, parent_return, *child_return;
    unsigned int nchildren_return;
    int ret;
    ret = XQueryTree(dpy, w, &root_return, &parent_return, &child_return,
                     &nchildren_return);

    if (child_return)
        XFree(child_return);

    return parent_return;
}

void grabbing_xinput_grab_start(Grabber *self)
{

	int count = XScreenCount(self->dpy);

	int screen;
	for (screen = 0; screen < count; screen++)
	{

		Window rootwindow = RootWindow(self->dpy, screen);

		if (self->is_direct_touch)
		{

			if (!self->button)
			{
				self->button = 1;
			}

			unsigned char mask_data[2] = {
				0,
			};
			XISetMask(mask_data, XI_ButtonPress);
			XISetMask(mask_data, XI_Motion);
			XISetMask(mask_data, XI_ButtonRelease);
			XIEventMask mask = {
				XIAllDevices, sizeof(mask_data), mask_data};

			int status = XIGrabDevice(self->dpy, self->deviceid, rootwindow,
									  CurrentTime, None,
									  GrabModeAsync,
									  GrabModeAsync, False, &mask);
		}
		else
		{

			if (!self->button)
			{
				self->button = 3;
			}

			unsigned char mask_data[2] = {
				0,
			};
			XISetMask(mask_data, XI_ButtonPress);
			XISetMask(mask_data, XI_Motion);
			XISetMask(mask_data, XI_ButtonRelease);
			XIEventMask mask = {
				XIAllDevices, sizeof(mask_data), mask_data};

			int nmods = 4;
			XIGrabModifiers mods[4] = {
				{0, 0},					 // no modifiers
				{LockMask, 0},			 // Caps lock
				{Mod2Mask, 0},			 // Num lock
				{LockMask | Mod2Mask, 0} // Caps & Num lock
			};

			nmods = 1;
			mods[0].modifiers = XIAnyModifier;

			int res = XIGrabButton(self->dpy, self->deviceid, self->button,
								   rootwindow, None,
								   GrabModeAsync, GrabModeAsync, False, &mask, nmods, mods);
		}
	}
}

void grabbing_xinput_grab_stop(Grabber *self)
{

	int count = XScreenCount(self->dpy);

	int screen;
	for (screen = 0; screen < count; screen++)
	{

		Window rootwindow = RootWindow(self->dpy, screen);

		if (self->is_direct_touch)
		{

			int status = XIUngrabDevice(self->dpy, self->deviceid, CurrentTime);
		}
		else
		{
			XIGrabModifiers modifiers[4] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
			XIGrabModifiers mods = {
				XIAnyModifier};
			XIUngrabButton(self->dpy, self->deviceid, self->button, rootwindow,
						   1, &mods);
		}
	}
}

static void mouse_click(Display *display, int button, int x, int y)
{

	XTestFakeMotionEvent(display, DefaultScreen(display), x, y, 0);
	XTestFakeButtonEvent(display, button, True, CurrentTime);
	XTestFakeButtonEvent(display, button, False, CurrentTime);
}

static Window get_window_under_pointer(Display *dpy)
{

    Window root_return, child_return;
    int root_x_return, root_y_return;
    int win_x_return, win_y_return;
    unsigned int mask_return;
    if (!XQueryPointer(dpy, DefaultRootWindow(dpy), &root_return, &child_return,
                  &root_x_return, &root_y_return, &win_x_return, &win_y_return,
                  &mask_return)) {
        return None;
    }

    if (child_return == None) {
        /* pointer is on root */
        return DefaultRootWindow(dpy);
    }

    /* climb parents to the top-level window (parent == root) */
    Window w = child_return;
    Window parent = get_parent_window(dpy, w);
    while (parent != None && parent != DefaultRootWindow(dpy)) {
        w = parent;
        parent = get_parent_window(dpy, w);
    }

    /* If the top-level window itself is not a client (no WM_STATE), try its children
       and return the first child that looks like the real client (has WM_STATE). */
    Atom wm_state = XInternAtom(dpy, "WM_STATE", False);
    Atom net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);

    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    if (XGetWindowProperty(dpy, w, wm_state, 0, 0, False, AnyPropertyType,
                           &actual_type, &actual_format, &nitems, &bytes_after,
                           &prop) == Success && prop) {
        XFree(prop);
        return w; /* top-level has WM_STATE, use it */
    }
    if (prop) { XFree(prop); prop = NULL; }

    /* query children and try to find a child with WM_STATE or _NET_WM_NAME */
    Window rootr, parentr, *children = NULL;
    unsigned int nchildren = 0;
    if (XQueryTree(dpy, w, &rootr, &parentr, &children, &nchildren)) {
        for (unsigned int i = 0; i < nchildren; ++i) {
            Window c = children[i];
            if (XGetWindowProperty(dpy, c, wm_state, 0, 0, False, AnyPropertyType,
                                   &actual_type, &actual_format, &nitems, &bytes_after,
                                   &prop) == Success && prop) {
                XFree(prop);
                XFree(children);
                return c;
            }
            if (prop) { XFree(prop); prop = NULL; }

            /* also try _NET_WM_NAME as a heuristic */
            if (XGetWindowProperty(dpy, c, net_wm_name, 0, 0, False, AnyPropertyType,
                                   &actual_type, &actual_format, &nitems, &bytes_after,
                                   &prop) == Success && prop) {
                XFree(prop);
                XFree(children);
                return c;
            }
            if (prop) { XFree(prop); prop = NULL; }
        }
        if (children) XFree(children);
    }

    return w;
}

static Window get_focused_window(Display *dpy)
{

	Window win = 0;
	int ret, val;
	ret = XGetInputFocus(dpy, &win, &val);

	if (val == RevertToParent)
	{
		win = get_parent_window(dpy, win);
	}

	return win;
}

static void execute_action(Display *dpy, Action *action, Window focused_window)
{
	int id;

	assert(dpy);
	assert(action);
	assert(focused_window);

	switch (action->type)
	{
	case ACTION_EXECUTE:
		id = fork();
		if (id == 0)
		{
			int i = system(action->original_str);
			exit(i);
		}
		if (id < 0)
		{
			fprintf(stderr, "Error forking.\n");
		}

		break;
	case ACTION_ICONIFY:
		action_iconify(dpy, focused_window);
		break;
	case ACTION_KILL:
		action_kill(dpy, focused_window);
		break;
	case ACTION_RAISE:
		action_raise(dpy, focused_window);
		break;
	case ACTION_LOWER:
		action_lower(dpy, focused_window);
		break;
	case ACTION_MAXIMIZE:
		action_maximize(dpy, focused_window);
		break;
	case ACTION_RESTORE:
		action_restore(dpy, focused_window);
		break;
	case ACTION_TOGGLE_MAXIMIZED:
		action_toggle_maximized(dpy, focused_window);
		break;
	case ACTION_KEYPRESS:
		action_keypress(dpy, action->original_str);
		break;
	default:
		fprintf(stderr, "found an unknown gesture \n");
	}

	XAllowEvents(dpy, 0, CurrentTime);

	return;
}

static void free_grabbed(Capture *free_me)
{
	assert(free_me);
	free(free_me->active_window_info);
	free(free_me);
}

static char get_fine_direction_from_deltas(int x_delta, int y_delta)
{

	if ((x_delta == 0) && (y_delta == 0))
	{
		return stroke_representations[NONE];
	}

	// check if the movement is near main axes
	if ((x_delta == 0) || (y_delta == 0) || (fabs((float)x_delta / (float)y_delta) > 3) || (fabs((float)y_delta / (float)x_delta) > 3))
	{

		// x axe
		if (abs(x_delta) > abs(y_delta))
		{

			if (x_delta > 0)
			{
				return stroke_representations[RIGHT];
			}
			else
			{
				return stroke_representations[LEFT];
			}

			// y axe
		}
		else
		{

			if (y_delta > 0)
			{
				return stroke_representations[DOWN];
			}
			else
			{
				return stroke_representations[UP];
			}
		}

		// diagonal axes
	}
	else
	{

		if (y_delta < 0)
		{
			if (x_delta < 0)
			{
				return stroke_representations[SEVEN];
			}
			else if (x_delta > 0)
			{ // RIGHT
				return stroke_representations[NINE];
			}
		}
		else if (y_delta > 0)
		{ // DOWN
			if (x_delta < 0)
			{ // RIGHT
				return stroke_representations[ONE];
			}
			else if (x_delta > 0)
			{
				return stroke_representations[THREE];
			}
		}
	}

	return stroke_representations[NONE];
}

static char get_direction_from_deltas(int x_delta, int y_delta)
{

	if (abs(y_delta) > abs(x_delta))
	{
		if (y_delta > 0)
		{
			return stroke_representations[DOWN];
		}
		else
		{
			return stroke_representations[UP];
		}
	}
	else
	{
		if (x_delta > 0)
		{
			return stroke_representations[RIGHT];
		}
		else
		{
			return stroke_representations[LEFT];
		}
	}
}

static void movement_add_direction(char *stroke_sequence, char direction)
{
	// grab stroke
	int len = strlen(stroke_sequence);
	if ((len == 0) || (stroke_sequence[len - 1] != direction))
	{

		if (len < MAX_STROKES_PER_CAPTURE)
		{

			stroke_sequence[len] = direction;
			stroke_sequence[len + 1] = '\0';
		}
	}
}

static int get_touch_status(XIDeviceInfo *device)
{

	int j = 0;

	for (j = 0; j < device->num_classes; j++)
	{
		XIAnyClassInfo *class = device->classes[j];
		XITouchClassInfo *t = (XITouchClassInfo *)class;

		if (class->type != XITouchClass)
			continue;

		if (t->mode == XIDirectTouch)
		{
			return 1;
		}
	}
	return 0;
}

static void grabber_xinput_open_devices(Grabber *self, int verbose)
{

	int ndevices;
	int i;
	XIDeviceInfo *device;
	XIDeviceInfo *devices;
	int deviceid = -1;
	devices = XIQueryDevice(self->dpy, XIAllDevices, &ndevices);
	if (verbose)
	{
		printf("\nXInput Devices:\n");
	}
	for (i = 0; i < ndevices; i++)
	{
		device = &devices[i];
		switch (device->use)
		{
		/// ṕointers
		case XIMasterPointer:
		case XISlavePointer:
		case XIFloatingSlave:
			if (strcasecmp(device->name, self->devicename) == 0)
			{
				if (verbose)
				{
					printf("   [x] '%s'\n", device->name);
				}
				self->deviceid = device->deviceid;
				self->is_direct_touch = get_touch_status(device);
			}
			else
			{
				if (verbose)
				{
					printf("   [ ] '%s'\n", device->name);
				}
			}
			break;
		case XIMasterKeyboard:
			//printf("master keyboard\n");
			break;
		case XISlaveKeyboard:
			//printf("slave keyboard\n");
			break;
		}
	}

	XIFreeDeviceInfo(devices);
}

/**
 * Clear previous movement data.
 */
void grabbing_start_movement(Grabber *self, int new_x, int new_y)
{

	self->started = 1;

	self->fine_direction_sequence[0] = '\0';
	self->rought_direction_sequence[0] = '\0';

	self->old_x = new_x;
	self->old_y = new_y;

	self->rought_old_x = new_x;
	self->rought_old_y = new_y;

	if (self->brush_image)
	{

		backing_save(&(self->backing), new_x - self->brush.image_width,
					 new_y - self->brush.image_height);
		brush_draw(&(self->brush), self->old_x, self->old_y);
	}
	return;
}

void grabbing_update_movement(Grabber *self, int new_x, int new_y)
{

	if (!self->started)
	{
		return;
	}

	// se for o caso, desenha o movimento na tela
	if (self->brush_image)
	{
		backing_save(&(self->backing), new_x - self->brush.image_width,
					 new_y - self->brush.image_height);

		brush_line_to(&(self->brush), new_x, new_y);
	}

	int x_delta = (new_x - self->old_x);
	int y_delta = (new_y - self->old_y);

	if ((abs(x_delta) > self->delta_min) || (abs(y_delta) > self->delta_min))
	{

		char stroke = get_fine_direction_from_deltas(x_delta, y_delta);

		movement_add_direction(self->fine_direction_sequence, stroke);

		// reset start position
		self->old_x = new_x;
		self->old_y = new_y;
	}

	int rought_delta_x = new_x - self->rought_old_x;
	int rought_delta_y = new_y - self->rought_old_y;

	char rought_direction = get_direction_from_deltas(rought_delta_x,
													  rought_delta_y);

	int square_distance_2 = rought_delta_x * rought_delta_x + rought_delta_y * rought_delta_y;

	if (self->delta_min * self->delta_min < square_distance_2)
	{
		// grab stroke

		movement_add_direction(self->rought_direction_sequence,
							   rought_direction);

		// reset start position
		self->rought_old_x = new_x;
		self->rought_old_y = new_y;
	}

	return;
}

/**
 *
 */
void grabbing_end_movement(Grabber *self, int new_x, int new_y,
						   char *device_name, Configuration *conf)
{

	grabbing_xinput_grab_stop(self);

	// Window focused_window = get_focused_window(self->dpy);
	Window focused_window = get_window_under_pointer(self->dpy);
	Window target_window = focused_window;

	Capture *grab = NULL;

	self->started = 0;

	// if is drawing
	if (self->brush_image)
	{
		backing_restore(&(self->backing));
	};

	// if there is no gesture
	if ((strlen(self->rought_direction_sequence) == 0) && (strlen(self->fine_direction_sequence) == 0))
	{

		if (!(self->synaptics))
		{

			printf("\nEmulating click\n");

			//grabbing_xinput_grab_stop(self);
			mouse_click(self->dpy, self->button, new_x, new_y);
			//grabbing_xinput_grab_start(self);
		}
	}
	else
	{

		int expression_count = 2;
		char **expression_list = malloc(sizeof(char *) * expression_count);

		expression_list[0] = self->fine_direction_sequence;
		expression_list[1] = self->rought_direction_sequence;

		ActiveWindowInfo *window_info = get_active_window_info(self->dpy,
															   target_window);

		grab = malloc(sizeof(Capture));

		grab->expression_count = expression_count;
		grab->expression_list = expression_list;
		grab->active_window_info = window_info;
	}

	if (grab)
	{

		printf("\n");
		printf("     Window title: \"%s\"\n", grab->active_window_info->title);
		printf("     Window class: \"%s\"\n", grab->active_window_info->class);
		printf("     Device      : \"%s\"\n", device_name);

		Gesture *gest = configuration_process_gesture(conf, grab);

		if (gest)
		{
			printf("     Movement '%s' matched gesture '%s' on context '%s'\n",
				   gest->movement->name, gest->name, gest->context->name);

			int j = 0;

			for (j = 0; j < gest->action_count; ++j)
			{
				Action *a = gest->action_list[j];
				printf("     Executing action: %s %s\n",
					   get_action_name(a->type), a->original_str);
				execute_action(self->dpy, a, target_window);
			}
		}
		else
		{

			for (int i = 0; i < grab->expression_count; ++i)
			{
				char *movement = grab->expression_list[i];
				printf(
					"     Sequence '%s' does not match any known movement.\n",
					movement);
			}
		}

		printf("\n");

		free_grabbed(grab);
	}

	grabbing_xinput_grab_start(self);
}

void grabber_set_button(Grabber *self, int button)
{
	self->button = button;
}

void grabber_set_device(Grabber *self, char *device_name)
{
	self->devicename = device_name;

	if (strcasecmp(self->devicename, "SYNAPTICS") == 0)
	{
		self->synaptics = 1;
		self->delta_min = 200;
	}
	else
	{
		self->synaptics = 0;
		self->delta_min = 30;
	}
}

void grabber_set_brush_color(Grabber *self, char *brush_color)
{
	self->brush_image = get_brush_image(brush_color);
}

Grabber *grabber_new(char *device_name, int button)
{

	Grabber *self = malloc(sizeof(Grabber));
	bzero(self, sizeof(Grabber));

	self->fine_direction_sequence = malloc(sizeof(char *) * 30);
	self->rought_direction_sequence = malloc(sizeof(char *) * 30);

	grabber_set_device(self, device_name);
	grabber_set_button(self, button);

	return self;
}

char *get_device_name_from_event(Grabber *self, XIDeviceEvent *data)
{
	int ndevices;
	char *device_name = NULL;
	XIDeviceInfo *device;
	XIDeviceInfo *devices;
	devices = XIQueryDevice(self->dpy, data->deviceid, &ndevices);
	if (ndevices == 1)
	{
		device = &devices[0];
		device_name = strdup(device->name);
	}

	return device_name;
}

void grabber_list_devices(Grabber *self)
{
	grabber_xinput_open_devices(self, True);
};

void grabber_xinput_loop(Grabber *self, Configuration *conf)
{

	XEvent ev;

	grabber_xinput_open_devices(self, False);
	grabbing_xinput_grab_start(self);

	while (!self->shut_down)
	{

		XNextEvent(self->dpy, &ev);

		if (ev.xcookie.type == GenericEvent && ev.xcookie.extension == self->opcode && XGetEventData(self->dpy, &ev.xcookie))
		{

			XIDeviceEvent *data = NULL;

			switch (ev.xcookie.evtype)
			{

			case XI_Motion:
				data = (XIDeviceEvent *)ev.xcookie.data;
				grabbing_update_movement(self, data->root_x, data->root_y);
				break;

			case XI_ButtonPress:
				data = (XIDeviceEvent *)ev.xcookie.data;
				grabbing_start_movement(self, data->root_x, data->root_y);
				break;

			case XI_ButtonRelease:
				data = (XIDeviceEvent *)ev.xcookie.data;

				char *device_name = get_device_name_from_event(self, data);

				grabbing_xinput_grab_stop(self);
				grabbing_end_movement(self, data->root_x, data->root_y,
									  device_name, conf);
				grabbing_xinput_grab_start(self);
				break;
			}
		}
		XFreeEventData(self->dpy, &ev.xcookie);
	}
}

void grabber_loop(Grabber *self, Configuration *conf)
{

	grabber_open_display(self);

	grabber_init_drawing(self);

	if (self->synaptics)
	{
		grabber_synaptics_loop(self, conf);
	}
	else
	{
		grabber_xinput_loop(self, conf);
	}

	printf("Grabbing loop finished for device '%s'.\n", self->devicename);
}

char *grabber_get_device_name(Grabber *self)
{
	return self->devicename;
}

void grabber_finalize(Grabber *self)
{
	if (self->brush_image)
	{
		brush_deinit(&(self->brush));
		backing_deinit(&(self->backing));
	}

	XCloseDisplay(self->dpy);
	return;
}
