/* -*-c-*- */
/* This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
/****************************************************************************
 * This module is based on Twm, but has been siginificantly modified
 * by Rob Nation
 ****************************************************************************/
/*****************************************************************************/
/**       Copyright 1988 by Evans & Sutherland Computer Corporation,        **/
/**                          Salt Lake City, Utah                           **/
/**  Portions Copyright 1989 by the Massachusetts Institute of Technology   **/
/**                        Cambridge, Massachusetts                         **/
/**                                                                         **/
/**                           All Rights Reserved                           **/
/**                                                                         **/
/**    Permission to use, copy, modify, and distribute this software and    **/
/**    its documentation  for  any  purpose  and  without  fee is hereby    **/
/**    granted, provided that the above copyright notice appear  in  all    **/
/**    copies and that both  that  copyright  notice  and  this  permis-    **/
/**    sion  notice appear in supporting  documentation,  and  that  the    **/
/**    names of Evans & Sutherland and M.I.T. not be used in advertising    **/
/**    in publicity pertaining to distribution of the  software  without    **/
/**    specific, written prior permission.                                  **/
/**                                                                         **/
/**    EVANS & SUTHERLAND AND M.I.T. DISCLAIM ALL WARRANTIES WITH REGARD    **/
/**    TO THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES  OF  MERCHANT-    **/
/**    ABILITY  AND  FITNESS,  IN  NO  EVENT SHALL EVANS & SUTHERLAND OR    **/
/**    M.I.T. BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL  DAM-    **/
/**    AGES OR  ANY DAMAGES WHATSOEVER  RESULTING FROM LOSS OF USE, DATA    **/
/**    OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER    **/
/**    TORTIOUS ACTION, ARISING OUT OF OR IN  CONNECTION  WITH  THE  USE    **/
/**    OR PERFORMANCE OF THIS SOFTWARE.                                     **/
/*****************************************************************************/

/* ---------------------------- included header files ----------------------- */

#include "config.h"

#if HAVE_SYS_BSDTYPES_H
#include <sys/bsdtypes.h>
#endif

#include <stdio.h>
#include <unistd.h>

#include "libs/ftime.h"
#include "libs/fvwmlib.h"
#include "libs/FShape.h"
#include "libs/PictureBase.h"
#include "libs/Colorset.h"
#include "fvwm.h"
#include "externs.h"
#include "cursor.h"
#include "functions.h"
#include "commands.h"
#include "bindings.h"
#include "misc.h"
#include "screen.h"
#include "events.h"
#include "eventhandler.h"
#include "eventmask.h"
#include "fvwmsignal.h"
#include "module_interface.h"
#include "session.h"
#include "borders.h"
#include "frame.h"
#include "add_window.h"
#include "icccm2.h"
#include "icons.h"
#include "gnome.h"
#include "ewmh.h"
#include "update.h"
#include "style.h"
#include "stack.h"
#include "geometry.h"
#include "focus.h"
#include "virtual.h"
#include "decorations.h"
#include "schedule.h"
#include "menus.h"
#include "colormaps.h"
#include "colorset.h"
#ifdef HAVE_STROKE
#include "stroke.h"
#endif /* HAVE_STROKE */

/* ---------------------------- local definitions --------------------------- */

#ifndef XUrgencyHint
#define XUrgencyHint            (1L << 8)
#endif

/*
** LASTEvent is the number of X events defined - it should be defined
** in X.h (to be like 35), but since extension (eg SHAPE) events are
** numbered beyond LASTEvent, we need to use a bigger number than the
** default, so let's undefine the default and use 256 instead.
*/
#undef LASTEvent
#define LASTEvent 256

/* ---------------------------- local macros -------------------------------- */

/* ---------------------------- imports ------------------------------------- */

extern void StartupStuff(void);

/* ---------------------------- included code files ------------------------- */

/* ---------------------------- local types --------------------------------- */

typedef void (*PFEH)(const evh_args_t *ea);

typedef struct
{
	Window w;
	Bool do_return_true;
	Bool do_return_true_cr;
	unsigned long cr_value_mask;
	Bool ret_does_match;
	unsigned long ret_type;
} check_if_event_args;

typedef struct
{
	unsigned do_forbid_function : 1;
	unsigned do_focus : 1;
	unsigned do_swallow_click : 1;
	unsigned do_raise : 1;
} hfrc_ret_t;

/* ---------------------------- forward declarations ------------------------ */

/* ---------------------------- local variables ----------------------------- */

static int Button = 0;
static const FvwmWindow *xcrossing_last_grab_window = NULL;
STROKE_CODE(static int send_motion);
STROKE_CODE(static char sequence[STROKE_MAX_SEQUENCE + 1]);
static PFEH EventHandlerJumpTable[LASTEvent];

/* ---------------------------- exported variables (globals) ---------------- */

int last_event_type = 0;
Window PressedW = None;
fd_set init_fdset;

/* ---------------------------- local functions ----------------------------- */

static void fake_map_unmap_notify(const FvwmWindow *fw, int event_type)
{
	XEvent client_event;
	XWindowAttributes winattrs = {0};

	if (!XGetWindowAttributes(dpy, FW_W(fw), &winattrs))
	{
		return;
	}
	XSelectInput(
		dpy, FW_W(fw),
		winattrs.your_event_mask & ~StructureNotifyMask);
	client_event.type = event_type;
	client_event.xmap.display = dpy;
	client_event.xmap.event = FW_W(fw);
	client_event.xmap.window = FW_W(fw);
	switch (event_type)
	{
	case MapNotify:
		client_event.xmap.override_redirect = False;
		break;
	case UnmapNotify:
		client_event.xunmap.from_configure = False;
		break;
	default:
		/* not possible if called correctly */
		break;
	}
	FSendEvent(
		dpy, FW_W(fw), False, StructureNotifyMask, &client_event);
	XSelectInput(dpy, FW_W(fw), winattrs.your_event_mask);

	return;
}

static Bool test_map_request(
	Display *display, XEvent *event, char *arg)
{
	check_if_event_args *cie_args;
	Bool rc;

	cie_args = (check_if_event_args *)arg;
	cie_args->ret_does_match = False;
	if (event->type == MapRequest &&
	    event->xmaprequest.window == cie_args->w)
	{
		cie_args->ret_type = MapRequest;
		cie_args->ret_does_match = True;
		rc = cie_args->do_return_true;
	}
	else
	{
		cie_args->ret_type = 0;
		rc = False;
	}

	/* Yes, it is correct that this function always returns False. */
	return rc;
}

static Bool test_resizing_event(
	Display *display, XEvent *event, char *arg)
{
	check_if_event_args *cie_args;
	Bool rc;

	cie_args = (check_if_event_args *)arg;
	cie_args->ret_does_match = False;
	if (event->xany.window != cie_args->w)
	{
		return False;
	}
	rc = False;
	switch (event->type)
	{
	case ConfigureRequest:
		if ((event->xconfigurerequest.value_mask &
		     cie_args->cr_value_mask) != 0)
		{
			cie_args->ret_type = ConfigureRequest;
			cie_args->ret_does_match = True;
			rc = cie_args->do_return_true_cr;
		}
		break;
	case PropertyNotify:
		if (event->xproperty.atom == XA_WM_NORMAL_HINTS)
		{
			cie_args->ret_type = PropertyNotify;
			cie_args->ret_does_match = True;
			rc = cie_args->do_return_true;
		}
	default:
		break;
	}

	/* Yes, it is correct that this function always returns False. */
	return rc;
}

/***********************************************************************
 *
 *  Procedure:
 *      SendConfigureNotify - inform a client window of its geometry.
 *
 *  The input (frame) geometry will be translated to client geometry
 *  before sending.
 *
 ************************************************************************/
void SendConfigureNotify(
	FvwmWindow *fw, int x, int y, unsigned int w, unsigned int h,
	int bw, Bool send_for_frame_too)
{
	XEvent client_event;
	size_borders b;

	if (!fw || IS_SHADED(fw))
	{
		return;
	}
	client_event.type = ConfigureNotify;
	client_event.xconfigure.display = dpy;
	client_event.xconfigure.event = FW_W(fw);
	client_event.xconfigure.window = FW_W(fw);
	get_window_borders(fw, &b);
	client_event.xconfigure.x = x + b.top_left.width;
	client_event.xconfigure.y = y + b.top_left.height;
	client_event.xconfigure.width = w - b.total_size.width;
	client_event.xconfigure.height = h - b.total_size.height;
	client_event.xconfigure.border_width = bw;
	client_event.xconfigure.above = FW_W_FRAME(fw);
	client_event.xconfigure.override_redirect = False;
	FSendEvent(
		dpy, FW_W(fw), False, StructureNotifyMask, &client_event);
	if (send_for_frame_too)
	{
		/* This is for buggy tk, which waits for the real
		 * ConfigureNotify on frame instead of the synthetic one on w.
		 * The geometry data in the event will not be correct for the
		 * frame, but tk doesn't look at that data anyway. */
		client_event.xconfigure.event = FW_W_FRAME(fw);
		client_event.xconfigure.window = FW_W_FRAME(fw);
		FSendEvent(
			dpy, FW_W_FRAME(fw), False, StructureNotifyMask,
			&client_event);
	}

	return;
}

/* Helper function for __handle_focus_raise_click(). */
static Bool __test_for_motion(int x0, int y0)
{
	int x;
	int y;
	unsigned int mask;

	/* Query the pointer to do this. We can't check for events here since
	 * the events are still needed if the pointer moves. */
	for (x = x0, y = y0; FQueryPointer(
		     dpy, Scr.Root, &JunkRoot, &JunkChild, &JunkX, &JunkY,
		     &x, &y, &mask) == True; usleep(20000))
	{
		if ((mask & DEFAULT_ALL_BUTTONS_MASK) == 0)
		{
			/* all buttons are released */
			return False;
		}
		else if (abs(x - x0) >= Scr.MoveThreshold ||
			 abs(y - y0) >= Scr.MoveThreshold)
		{
			/* the pointer has moved */
			return True;
		}
	}

	/* pointer has moved off screen */
	return True;
}

/* Helper function for __handle_focus_raise_click(). */
static void __check_click_to_focus_or_raise(
	hfrc_ret_t *ret_args, const exec_context_t *exc)
{
	FvwmWindow * const fw = exc->w.fw;
	const XEvent *te = exc->x.etrigger;
	struct
	{
		unsigned is_client_click : 1;
		unsigned is_focused : 1;
	} f;

	f.is_focused = !!focus_is_focused(fw);
	f.is_client_click = (exc->w.wcontext == C_WINDOW) ? True : False;
	/* check if we need to raise and/or focus the window */
	ret_args->do_focus = focus_query_click_to_focus(fw, exc->w.wcontext);
	if (exc->w.wcontext == C_WINDOW && !ret_args->do_focus &&
	    !f.is_focused && FP_DO_FOCUS_BY_PROGRAM(FW_FOCUS_POLICY(fw)) &&
	    !fpol_query_allow_user_focus(&FW_FOCUS_POLICY(fw)))
	{
		/* Give the window a chance to to take focus itself */
		ret_args->do_focus = 1;
	}
	if (ret_args->do_focus && focus_is_focused(fw))
	{
		ret_args->do_focus = 0;
	}
 	ret_args->do_raise =
		focus_query_click_to_raise(fw, f.is_focused, exc->w.wcontext);
#define EXPERIMENTAL_ROU_HANDLING_V2
#ifdef EXPERIMENTAL_ROU_HANDLING_V2
/*  RBW -- Dang! This works without the one in HandleEnterNotify!  */
	if (ret_args->do_raise && is_on_top_of_layer_and_above_unmanaged(fw))
#else
	if (ret_args->do_raise && is_on_top_of_layer(fw))
#endif
	{
		ret_args->do_raise = 0;
	}
	if ((ret_args->do_focus &&
	     FP_DO_IGNORE_FOCUS_CLICK_MOTION(FW_FOCUS_POLICY(fw))) ||
	    (ret_args->do_raise &&
	     FP_DO_IGNORE_RAISE_CLICK_MOTION(FW_FOCUS_POLICY(fw))))
	{
		/* Pass further events to the application and check if a button
		 * release or motion event occurs next.  If we don't do this
		 * here, the pointer will seem to be frozen in
		 * __test_for_motion(). */
		XAllowEvents(dpy, ReplayPointer, CurrentTime);
		if (__test_for_motion(te->xbutton.x_root, te->xbutton.y_root))
		{
			/* the pointer was moved, process event normally */
			ret_args->do_focus = 0;
			ret_args->do_raise = 0;
		}
	}
	if (ret_args->do_focus || ret_args->do_raise)
	{
		if (!((ret_args->do_focus &&
		       FP_DO_ALLOW_FUNC_FOCUS_CLICK(FW_FOCUS_POLICY(fw))) ||
		      (ret_args->do_raise &&
		       FP_DO_ALLOW_FUNC_RAISE_CLICK(FW_FOCUS_POLICY(fw)))))
		{
			ret_args->do_forbid_function = 1;
		}
		if (!((ret_args->do_focus &&
		       FP_DO_PASS_FOCUS_CLICK(FW_FOCUS_POLICY(fw))) ||
		      (ret_args->do_raise &&
		       FP_DO_PASS_RAISE_CLICK(FW_FOCUS_POLICY(fw)))))
		{
			ret_args->do_swallow_click = 1;
		}
	}

	return;
}

/* Finds out if the click on a window must be used to focus or raise it. */
static void __handle_focus_raise_click(
	hfrc_ret_t *ret_args, const exec_context_t *exc)
{
	memset (ret_args, 0, sizeof(*ret_args));
	if (exc->w.fw == NULL)
	{
		return;
	}
	/* check for proper click button and modifiers*/
	if (FP_USE_MOUSE_BUTTONS(FW_FOCUS_POLICY(exc->w.fw)) != 0 &&
	    !(FP_USE_MOUSE_BUTTONS(FW_FOCUS_POLICY(exc->w.fw)) &
	      (1 << (exc->x.etrigger->xbutton.button - 1))))
	{
		/* wrong button, handle click normally */
		return;
	}
	else if (FP_USE_MODIFIERS(FW_FOCUS_POLICY(exc->w.fw)) !=
		 FPOL_ANY_MODIFIER &&
		 MaskUsedModifiers(
			 FP_USE_MODIFIERS(FW_FOCUS_POLICY(exc->w.fw))) !=
		 MaskUsedModifiers(exc->x.etrigger->xbutton.state))
	{
		/* right button but wrong modifiers, handle click normally */
		return;
	}
	else
	{
		__check_click_to_focus_or_raise(ret_args, exc);
	}

	return;
}

/* Helper function for HandleButtonPress */
static Bool __is_bpress_window_handled(const exec_context_t *exc)
{
	Window eventw;
	const XEvent *te = exc->x.etrigger;

	if (exc->w.fw == NULL)
	{
		if (te->xbutton.window != Scr.Root &&
		    !is_pan_frame(te->xbutton.window))
		{
			/* Ignore events in unmanaged windows or subwindows of
			 * a client */
			return False;
		}
		else
		{
			return True;
		}
	}
	eventw = (te->xbutton.subwindow != None &&
		  te->xany.window != FW_W(exc->w.fw)) ?
		te->xbutton.subwindow : te->xany.window;
	if (is_frame_hide_window(eventw) || eventw == FW_W_FRAME(exc->w.fw))
	{
		return False;
	}
	if (!XGetGeometry(
		    dpy, eventw, &JunkRoot, &JunkX, &JunkY, &JunkWidth,
		    &JunkHeight, &JunkBW, &JunkDepth))
	{
		/* The window has already died. */
		return False;
	}

	return True;
}

/* Helper function for __handle_bpress_on_managed */
static Bool __handle_click_to_focus(const exec_context_t *exc)
{
	fpol_set_focus_by_t set_by;

	switch (exc->w.wcontext)
	{
	case C_WINDOW:
		set_by = FOCUS_SET_BY_CLICK_CLIENT;
		break;
	case C_ICON:
		set_by = FOCUS_SET_BY_CLICK_ICON;
		break;
	default:
		set_by = FOCUS_SET_BY_CLICK_DECOR;
		break;
	}
	SetFocusWindow(exc->w.fw, True, set_by);
	focus_grab_buttons(exc->w.fw);
	if (focus_is_focused(exc->w.fw) && !IS_ICONIFIED(exc->w.fw))
	{
		border_draw_decorations(
			exc->w.fw, PART_ALL, True, True, CLEAR_ALL, NULL, NULL);
	}

	return focus_is_focused(exc->w.fw);
}

/* Helper function for __handle_bpress_on_managed */
static Bool __handle_click_to_raise(const exec_context_t *exc)
{
	Bool rc = False;
	int is_focused;

	is_focused = focus_is_focused(exc->w.fw);
	if (focus_query_click_to_raise(exc->w.fw, is_focused, True))
	{
		rc = True;
	}

	return rc;
}

/* Helper function for HandleButtonPress */
static void __handle_bpress_stroke(void)
{
	STROKE_CODE(stroke_init());
	STROKE_CODE(send_motion = True);

	return;
}

/* Helper function for __handle_bpress_on_managed */
static Bool __handle_bpress_action(
	const exec_context_t *exc, char *action)
{
	window_parts part;
	Bool do_force;
	Bool rc = False;

	if (!action || *action == 0)
	{
		PressedW = None;
		return False;
	}
	/* draw pressed in decorations */
	part = border_context_to_parts(exc->w.wcontext);
	do_force = (part & PART_TITLEBAR) ? True : False;
	border_draw_decorations(
		exc->w.fw, part, (Scr.Hilite == exc->w.fw), do_force,
		CLEAR_ALL, NULL, NULL);
	/* execute the action */
	if (IS_ICONIFIED(exc->w.fw))
	{
		/* release the pointer since it can't do harm over an icon */
		XAllowEvents(dpy, AsyncPointer, CurrentTime);
	}
	execute_function(NULL, exc, action, 0);
	if (exc->w.wcontext != C_WINDOW && exc->w.wcontext != C_NO_CONTEXT)
	{
		WaitForButtonsUp(True);
		rc = True;
	}
	/* redraw decorations */
	PressedW = None;
	if (check_if_fvwm_window_exists(exc->w.fw))
	{
		part = border_context_to_parts(exc->w.wcontext);
		do_force = (part & PART_TITLEBAR) ? True : False;
		border_draw_decorations(
			exc->w.fw, part, (Scr.Hilite == exc->w.fw), do_force,
			CLEAR_ALL, NULL, NULL);
	}

	return rc;
}

/* Handles button presses on the root window. */
static void __handle_bpress_on_root(const exec_context_t *exc)
{
	char *action;

	PressedW = None;
	__handle_bpress_stroke();
	/* search for an appropriate mouse binding */
	action = CheckBinding(
		Scr.AllBindings, STROKE_ARG(0) exc->x.etrigger->xbutton.button,
		exc->x.etrigger->xbutton.state, GetUnusedModifiers(), C_ROOT,
		BIND_BUTTONPRESS);
	if (action && *action)
	{
		const exec_context_t *exc2;
		exec_context_changes_t ecc;

		ecc.w.wcontext = C_ROOT;
		exc2 = exc_clone_context(exc, &ecc, ECC_WCONTEXT);
		execute_function(NULL, exc2, action, 0);
		exc_destroy_context(exc2);
		WaitForButtonsUp(True);
	}
	else
	{
		/* do gnome buttonpress forwarding if win == root */
		GNOME_ProxyButtonEvent(exc->x.etrigger);
	}

	return;
}

/* Handles button presses on unmanaged windows */
static void __handle_bpress_on_unmanaged(const exec_context_t *exc)
{
	/* Pass the event to the application. */
	XAllowEvents(dpy, ReplayPointer, CurrentTime);
	XFlush(dpy);

	return;
}

/* Handles button presses on managed windows */
static void __handle_bpress_on_managed(const exec_context_t *exc)
{
	char *action;
	hfrc_ret_t f;
	FvwmWindow * const fw = exc->w.fw;
	XEvent *e;

	e = exc->x.etrigger;
	PressedW = exc->w.w;
	/* Now handle click to focus and click to raise. */
	__handle_focus_raise_click(&f, exc);
	if (f.do_focus)
	{
		if (!__handle_click_to_focus(exc))
		{
			/* Window didn't accept the focus; pass the click to the
			 * application. */
			f.do_swallow_click = 0;
		}
	}
	if (f.do_raise)
	{
		if (__handle_click_to_raise(exc) == True)
		{
			/* We can't raise the window immediately because the
			 * action bound to the click might be "Lower" or
			 * "RaiseLower". So mark the window as scheduled to be
			 * raised after the binding is executed. Functions that
			 * modify the stacking order will reset this flag. */
			SET_SCHEDULED_FOR_RAISE(fw, 1);
		}
	}
	/* handle bindings */
	if (!f.do_forbid_function)
	{
		/* stroke bindings */
		__handle_bpress_stroke();
		/* mouse bindings */
		action = CheckBinding(
			Scr.AllBindings, STROKE_ARG(0) e->xbutton.button,
			e->xbutton.state, GetUnusedModifiers(), exc->w.wcontext,
			BIND_BUTTONPRESS);
		if (__handle_bpress_action(exc, action))
		{
			f.do_swallow_click = 1;
		}
	}
	/* raise the window */
	if (IS_SCHEDULED_FOR_RAISE(fw))
	{
		/* Now that we know the action did not restack the window we
		 * can raise it.
		 * dv (10-Aug-2002):  We can safely raise the window after
		 * redrawing it since all the decorations are drawn in the
		 * window background and no Expose event is generated. */
		RaiseWindow(fw);
		SET_SCHEDULED_FOR_RAISE(fw, 0);
	}
	/* clean up */
	if (!f.do_swallow_click)
	{
		/* pass the click to the application */
		XAllowEvents(dpy, ReplayPointer, CurrentTime);
		XFlush(dpy);
	}
	else if (f.do_focus || f.do_raise)
	{
		WaitForButtonsUp(True);
	}

	return;
}

/* ---------------------------- event handlers ------------------------------ */

void HandleButtonPress(const evh_args_t *ea)
{
	DBUG("HandleButtonPress","Routine Entered");

	GrabEm(CRS_NONE, GRAB_PASSIVE);
	if (__is_bpress_window_handled(ea->exc) == False)
	{
		__handle_bpress_on_unmanaged(ea->exc);
	}
	else if (ea->exc->w.fw != NULL)
	{
		__handle_bpress_on_managed(ea->exc);
	}
	else
	{
		__handle_bpress_on_root(ea->exc);
	}
	UngrabEm(GRAB_PASSIVE);

	return;
}

#ifdef HAVE_STROKE
void HandleButtonRelease(const evh_args_t *ea)
{
	char *action;
	int real_modifier;
	const XEvent *te = ea->exc->x.etrigger;

	DBUG("HandleButtonRelease","Routine Entered");

	send_motion = False;
	stroke_trans (sequence);

	DBUG("HandleButtonRelease",sequence);

	/*  Allows modifier to work (Only R context works here). */
	real_modifier = te->xbutton.state - (1 << (7 + te->xbutton.button));
	/* need to search for an appropriate stroke binding */
	action = CheckBinding(
		Scr.AllBindings, sequence, te->xbutton.button, real_modifier,
		GetUnusedModifiers(), ea->exc->w.wcontext, BIND_STROKE);
	/* got a match, now process it */
	if (action != NULL && (action[0] != 0))
	{
		execute_function(NULL, ea->exc, action, 0);
		WaitForButtonsUp(True);
	}
	else
	{
		/*
		 * do gnome buttonpress forwarding if win == root
		 */
		if (Scr.Root == te->xany.window)
		{
			GNOME_ProxyButtonEvent(te);
		}
	}

	return;
}
#endif /* HAVE_STROKE */

void HandleClientMessage(const evh_args_t *ea)
{
	const XEvent *te = ea->exc->x.etrigger;
	FvwmWindow * const fw = ea->exc->w.fw;

	DBUG("HandleClientMessage","Routine Entered");

	/* Process GNOME and EWMH Messages */
	if (GNOME_ProcessClientMessage(ea->exc))
	{
		return;
	}
	else if (EWMH_ProcessClientMessage(ea->exc))
	{
		return;
	}

	/* handle deletion of tear out menus */
	if (fw && IS_TEAR_OFF_MENU(fw) && te->xclient.format == 32 &&
	    te->xclient.data.l[0] == _XA_WM_DELETE_WINDOW)
	{
		menu_close_tear_off_menu(fw);
		return;
	}

	if (te->xclient.message_type == _XA_WM_CHANGE_STATE &&
	    fw && te->xclient.data.l[0] == IconicState && !IS_ICONIFIED(fw))
	{
		const exec_context_t *exc;
		exec_context_changes_t ecc;

		ecc.w.wcontext = C_WINDOW;
		exc = exc_clone_context(ea->exc, &ecc, ECC_WCONTEXT);
		execute_function(NULL, exc, "Iconify", 0);
		exc_destroy_context(exc);
		return;
	}

	/* FIXME: Is this safe enough ? I guess if clients behave
	   according to ICCCM and send these messages only if they
	   when grabbed the pointer, it is OK */
	{
		extern Atom _XA_WM_COLORMAP_NOTIFY;
		if (te->xclient.message_type == _XA_WM_COLORMAP_NOTIFY)
		{
			set_client_controls_colormaps(te->xclient.data.l[1]);
			return;
		}
	}

	/*
	** CKH - if we get here, it was an unknown client message, so send
	** it to the client if it was in a window we know about.  I'm not so
	** sure this should be done or not, since every other window manager
	** I've looked at doesn't.  But it might be handy for a free drag and
	** drop setup being developed for Linux.
	*/
	if (fw)
	{
		if (te->xclient.window != FW_W(fw))
		{
			XEvent e;

			e = *te;
			e.xclient.window = FW_W(fw);
			FSendEvent(dpy, FW_W(fw), False, NoEventMask, &e);
		}
	}
}

void HandleColormapNotify(const evh_args_t *ea)
{
	colormap_handle_colormap_notify(ea);

	return;
}

void HandleConfigureRequest(const evh_args_t *ea)
{
	rectangle new_g;
	int dx;
	int dy;
	int dw;
	int dh;
	int constr_w;
	int constr_h;
	int oldnew_w;
	int oldnew_h;
	XWindowChanges xwc;
	unsigned long xwcm;
	Bool do_send_event = False;
	int cn_count = 0;
	const XEvent *te = ea->exc->x.etrigger;
	XConfigureRequestEvent cre;
	FvwmWindow *fw = ea->exc->w.fw;

	DBUG("HandleConfigureRequest","Routine Entered");

	cre = te->xconfigurerequest;
	/* te->xany.window is te->.xconfigurerequest.parent, so the context
	 * window may be wrong. */
	if (XFindContext(dpy, cre.window, FvwmContext, (caddr_t *)&fw) ==
	    XCNOENT)
	{
		fw = NULL;
	}

	/*
	 * According to the July 27, 1988 ICCCM draft, we should ignore size
	 * and position fields in the WM_NORMAL_HINTS property when we map a
	 * window. Instead, we'll read the current geometry.  Therefore, we
	 * should respond to configuration requests for windows which have
	 * never been mapped.
	 */
	if (!fw || cre.window == FW_W_ICON_TITLE(fw) ||
	    cre.window == FW_W_ICON_PIXMAP(fw))
	{
		xwcm = cre.value_mask &
			(CWX | CWY | CWWidth | CWHeight | CWBorderWidth);
		xwc.x = cre.x;
		xwc.y = cre.y;
		if (fw && FW_W_ICON_PIXMAP(fw) == cre.window)
		{
			set_icon_picture_size(
				fw, cre.width + 2 * cre.border_width,
				cre.height + 2 * cre.border_width);
		}
		if (fw)
		{
			set_icon_position(fw, cre.x, cre.y);
			broadcast_icon_geometry(fw, False);
		}
		xwc.width = cre.width;
		xwc.height = cre.height;
		xwc.border_width = cre.border_width;

		XConfigureWindow(dpy, cre.window, xwcm, &xwc);

		if (fw)
		{
			if (cre.window != FW_W_ICON_PIXMAP(fw) &&
			    FW_W_ICON_PIXMAP(fw) != None)
			{
				rectangle g;

				get_icon_picture_geometry(fw, &g);
				xwc.x = g.x;
				xwc.x = g.y;
				xwcm = cre.value_mask & (CWX | CWY);
				XConfigureWindow(
					dpy, FW_W_ICON_PIXMAP(fw), xwcm, &xwc);
			}
			if (FW_W_ICON_TITLE(fw) != None)
			{
				rectangle g;

				get_icon_title_geometry(fw, &g);
				xwc.x = g.x;
				xwc.x = g.y;
				xwcm = cre.value_mask & (CWX | CWY);
				XConfigureWindow(
					dpy, FW_W_ICON_TITLE(fw), xwcm, &xwc);
			}
		}
		if (!fw)
		{
			return;
		}
	}

	if (FHaveShapeExtension && FShapesSupported)
	{
		int i;
		unsigned int u;
		Bool b;
		int boundingShaped;

		/* suppress compiler warnings w/o shape extension */
		i = 0;
		u = 0;
		b = False;

		if (FShapeQueryExtents(
			    dpy, FW_W(fw), &boundingShaped, &i, &i, &u, &u, &b,
			    &i, &i, &u, &u))
		{
			fw->wShaped = boundingShaped;
		}
		else
		{
			fw->wShaped = 0;
		}
	}

	/*  Stacking order change requested...  */
	/*  Handle this *after* geometry changes, since we need the new
	    geometry in occlusion calculations */
	if ((cre.value_mask & CWStackMode) && !DO_IGNORE_RESTACK(fw))
	{
		FvwmWindow *otherwin = NULL;

		if (cre.value_mask & CWSibling)
		{
			if (XFindContext(dpy, cre.above, FvwmContext,
					 (caddr_t *) &otherwin) == XCNOENT)
			{
				otherwin = NULL;
			}
			if (otherwin == fw)
			{
				otherwin = NULL;
			}
		}
		if ((cre.detail != Above) && (cre.detail != Below))
		{
			HandleUnusualStackmodes(
				cre.detail, fw, cre.window, otherwin,
				cre.above);
		}
		/* only allow clients to restack windows within their layer */
		else if (!otherwin || compare_window_layers(otherwin, fw) != 0)
		{
			switch (cre.detail)
			{
			case Above:
				RaiseWindow(fw);
				break;
			case Below:
				LowerWindow(fw);
				break;
			}
		}
		else
		{
			xwc.sibling = FW_W_FRAME(otherwin);
			xwc.stack_mode = cre.detail;
			xwcm = CWSibling | CWStackMode;
			XConfigureWindow(dpy, FW_W_FRAME(fw), xwcm, &xwc);

			/* Maintain the condition that icon windows are stacked
			   immediately below their frame */
			/* 1. for fw */
			xwc.sibling = FW_W_FRAME(fw);
			xwc.stack_mode = Below;
			xwcm = CWSibling | CWStackMode;
			if (FW_W_ICON_TITLE(fw) != None)
			{
				XConfigureWindow(
					dpy, FW_W_ICON_TITLE(fw), xwcm, &xwc);
			}
			if (FW_W_ICON_PIXMAP(fw) != None)
			{
				XConfigureWindow(
					dpy, FW_W_ICON_PIXMAP(fw), xwcm, &xwc);
			}

			/* 2. for otherwin */
			if (cre.detail == Below)
			{
				xwc.sibling = FW_W_FRAME(otherwin);
				xwc.stack_mode = Below;
				xwcm = CWSibling | CWStackMode;
				if (FW_W_ICON_TITLE(otherwin) != None)
				{
					XConfigureWindow(
						dpy, FW_W_ICON_TITLE(otherwin),
						xwcm, &xwc);
				}
				if (FW_W_ICON_PIXMAP(otherwin) != None)
				{
					XConfigureWindow(
						dpy, FW_W_ICON_PIXMAP(otherwin),
						xwcm, &xwc);
				}
			}

			/* Maintain the stacking order ring */
			if (cre.detail == Above)
			{
				remove_window_from_stack_ring(fw);
				add_window_to_stack_ring_after(
					fw, get_prev_window_in_stack_ring(
						otherwin));
			}
			else /* cre.detail == Below */
			{
				remove_window_from_stack_ring(fw);
				add_window_to_stack_ring_after(fw, otherwin);
			}

			/*
			  Let the modules know that fw changed its place
			  in the stacking order
			*/
			BroadcastRestackThisWindow(fw);
		}
		/* srt (28-Apr-2001): Tk needs a ConfigureNotify event after a
		 * raise, otherwise it would hang for two seconds */
		new_g = fw->frame_g;
		do_send_event = True;
	}

	if (fw != NULL && cre.window == FW_W(fw))
	{
		size_borders b;

#define EXPERIMENTAL_ANTI_RACE_CONDITION_CODE
		/* This is not a good idea because this interferes with changes
		 * in the size hints of the window.  However, it is impossible
		 * to be completely safe here. For example, if the client
		 * changes the size inc, then resizes the size of its window
		 * and then changes the size inc again - all in one batch -
		 * then the WM will read the *second* size inc upon the *first*
		 * event and use the wrong one in the ConfigureRequest
		 * calculations. */
		/* dv (31 Mar 2002): The code now handles these
		 * situations, so enable it again. */
#ifdef EXPERIMENTAL_ANTI_RACE_CONDITION_CODE
		/* merge all pending ConfigureRequests for the window into a
		 * single event */
		XEvent e;
		XConfigureRequestEvent *ecre;
		check_if_event_args args;

		args.w = cre.window;
		args.do_return_true = False;
		args.do_return_true_cr = True;
		args.cr_value_mask =
			(CWX | CWY | CWWidth | CWHeight | CWBorderWidth);
		args.ret_does_match = False;
		args.ret_type = 0;
#if 0
		/* free some CPU */
		/* dv (7 May 2002): No, it's better to not reschedule processes
		 * here because some funny applications (XMMS, GTK) seem to
		 * expect that ConfigureRequests are handled instantly or they
		 * freak out. */
		usleep(1);
#endif
		for (cn_count = 0; 1; )
		{
			unsigned long vma;
			unsigned long vmo;
			unsigned long xm;
			unsigned long ym;
			evh_args_t ea2;
			exec_context_changes_t ecc;

			FCheckIfEvent(
				dpy, &e, test_resizing_event, (char *)&args);
			if (args.ret_does_match == False)
			{
				break;
			}
			else if (args.ret_type == PropertyNotify)
			{
				/* Can't merge events with a PropertyNotify in
				 * between.  The event is still on the queue.
				 */
				break;
			}
			else if (args.ret_type != ConfigureRequest)
			{
				/* not good.  unselected event type! */
				continue;
			}
			/* Event was removed from the queue and stored in e. */
			xm = CWX | CWWidth;
			ym = CWY | CWHeight;
			ecre = &e.xconfigurerequest;
			vma = cre.value_mask & ecre->value_mask;
			vmo = cre.value_mask | ecre->value_mask;
			if (((vma & xm) == 0 && (vmo & xm) == xm) ||
			    ((vma & ym) == 0 && (vmo & ym) == ym))
			{
				/* can't merge events since location of window
				 * might get screwed up. */
				FPutBackEvent(dpy, &e);
				break;
			}
			/* partially handle the event */
			ecre->value_mask &= ~args.cr_value_mask;

			ea2.exc = exc_clone_context(
				ea->exc, &ecc, ECC_ETRIGGER);
			HandleConfigureRequest(&ea2);
			exc_destroy_context(ea2.exc);


			/* collect the size/position changes */
			if (ecre->value_mask & CWX)
			{
				cre.value_mask |= CWX;
				cre.x = ecre->x;
			}
			if (ecre->value_mask & CWY)
			{
				cre.value_mask |= CWY;
				cre.y = ecre->y;
			}
			if (ecre->value_mask & CWWidth)
			{
				cre.value_mask |= CWWidth;
				cre.width = ecre->width;
			}
			if (ecre->value_mask & CWHeight)
			{
				cre.value_mask |= CWHeight;
				cre.height = ecre->height;
			}
			if (ecre->value_mask & CWBorderWidth)
			{
				cre.value_mask |= CWBorderWidth;
				cre.border_width = ecre->border_width;
			}
			cn_count++;
		}
#endif

#if 0
		fprintf(stderr,
			"cre: %d(%d) %d(%d) %d(%d)x%d(%d) fw 0x%08x w 0x%08x "
			"ew 0x%08x  '%s'\n",
			cre.x, (int)(cre.value_mask & CWX),
			cre.y, (int)(cre.value_mask & CWY),
			cre.width, (int)(cre.value_mask & CWWidth),
			cre.height, (int)(cre.value_mask & CWHeight),
			(int)FW_W_FRAME(fw), (int)FW_W(fw), (int)cre.window,
			(fw->name.name) ? fw->name.name : "");
#endif
		/* Don't modify frame_FW fields before calling SetupWindow! */
		dx = 0;
		dy = 0;
		dw = 0;
		dh = 0;

		if (HAS_NEW_WM_NORMAL_HINTS(fw))
		{
			/* get the latest size hints */
			XSync(dpy, 0);
			GetWindowSizeHints(fw);
			SET_HAS_NEW_WM_NORMAL_HINTS(fw, 0);
		}
		if (IS_SHADED(fw) ||
		    !is_function_allowed(F_MOVE, NULL, fw, False, False))
		{
			/* forbid shaded applications to move their windows */
			cre.value_mask &= ~(CWX | CWY);
			/* resend the old geometry */
			do_send_event = True;
		}
		if (IS_MAXIMIZED(fw))
		{
			/* dont allow clients to resize maximized windows */
			cre.value_mask &= ~(CWWidth | CWHeight);
			/* resend the old geometry */
			do_send_event = True;
			dw = fw->max_g_defect.width;
			dh = fw->max_g_defect.width;
			dw = 0;
			dh = 0;
		}
		else if(!is_function_allowed(F_RESIZE, NULL, fw, False, False))
		{
			cre.value_mask &= ~(CWWidth | CWHeight);
			do_send_event = True;
		}

		/* for restoring */
		if (cre.value_mask & CWBorderWidth)
		{
			fw->attr_backup.border_width = cre.border_width;
		}
		/* override even if border change */
		get_window_borders(fw, &b);
		if (cre.value_mask & CWX)
		{
			dx = cre.x - fw->frame_g.x - b.top_left.width;
		}
		if (cre.value_mask & CWY)
		{
			dy = cre.y - fw->frame_g.y - b.top_left.height;
		}
		if (cre.value_mask & CWHeight)
		{
			if (cre.height <
			    (WINDOW_FREAKED_OUT_SIZE - b.total_size.height))
			{
				dh = cre.height - (fw->frame_g.height -
						    b.total_size.height);
			}
			else
			{
				/* patch to ignore height changes to
				 * astronomically large windows (needed for
				 * XEmacs 20.4); don't care if the window is
				 * shaded here - we won't use 'height' in this
				 * case anyway */
				/* inform the buggy app about the size that
				 * *we* want */
				do_send_event = True;
				dh = 0;
			}
		}
		if (cre.value_mask & CWWidth)
		{
			if (cre.width < (WINDOW_FREAKED_OUT_SIZE -
					  b.total_size.width))
			{
				dw = cre.width - (fw->frame_g.width -
						   b.total_size.width);
			}
			else
			{
				/* see above */
				do_send_event = True;
				dw = 0;
			}
		}

		/* SetupWindow (x,y) are the location of the upper-left outer
		 * corner and are passed directly to XMoveResizeWindow (frame).
		 *  The (width,height) are the inner size of the frame.  The
		 * inner width is the same as the requested client window
		 * width; the inner height is the same as the requested client
		 * window height plus any title bar slop. */
		new_g = fw->frame_g;
		if (IS_SHADED(fw))
		{
			new_g.width = fw->normal_g.width;
			new_g.height = fw->normal_g.height;
		}
		oldnew_w = new_g.width + dw;
		oldnew_h = new_g.height + dh;
		constr_w = oldnew_w;
		constr_h = oldnew_h;
		constrain_size(
			fw, NULL, (unsigned int *)&constr_w,
			(unsigned int *)&constr_h, 0, 0, CS_UPDATE_MAX_DEFECT);
		dw += (constr_w - oldnew_w);
		dh += (constr_h - oldnew_h);
		if ((cre.value_mask & CWX) && dw)
		{
			new_g.x = fw->frame_g.x + dx;
			new_g.width = fw->frame_g.width + dw;
		}
		else if ((cre.value_mask & CWX) && !dw)
		{
			new_g.x = fw->frame_g.x + dx;
		}
		else if (!(cre.value_mask & CWX) && dw)
		{
			gravity_resize(fw->hints.win_gravity, &new_g, dw, 0);
		}
		if ((cre.value_mask & CWY) && dh)
		{
			new_g.y = fw->frame_g.y + dy;
			new_g.height = fw->frame_g.height + dh;
		}
		else if ((cre.value_mask & CWY) && !dh)
		{
			new_g.y = fw->frame_g.y + dy;
		}
		else if (!(cre.value_mask & CWY) && dh)
		{
			gravity_resize(fw->hints.win_gravity, &new_g, 0, dh);
		}

		if (new_g.x == fw->frame_g.x && new_g.y == fw->frame_g.y &&
		    new_g.width == fw->frame_g.width &&
		    new_g.height == fw->frame_g.height)
		{
			/* Window will not be moved or resized; send a
			 * synthetic ConfigureNotify. */
			do_send_event = True;
		}
		else if ((cre.value_mask & CWX) || (cre.value_mask & CWY) ||
			 dw || dh)
		{
			if (IS_SHADED(fw))
			{
				get_shaded_geometry(fw, &new_g, &new_g);
			}
			frame_setup_window_app_request(
				fw, new_g.x, new_g.y, new_g.width,
				new_g.height, False);
			/* make sure the window structure has the new position
			 */
			update_absolute_geometry(fw);
			maximize_adjust_offset(fw);
			GNOME_SetWinArea(fw);
		}
		else if (DO_FORCE_NEXT_CR(fw))
		{
			new_g = fw->frame_g;
			do_send_event = True;
		}
		SET_FORCE_NEXT_CR(fw, 0);
		SET_FORCE_NEXT_PN(fw, 0);
	}
#if 1
	/* This causes some ddd windows not to be drawn properly. Reverted back
	 * to the old method in frame_setup_window. */
	/* domivogt (15-Oct-1999): enabled this to work around buggy apps that
	 * ask for a nonsense height and expect that they really get it. */
	if (do_send_event)
	{
		cn_count++;
	}
	else if (cn_count > 0)
	{
		do_send_event = 1;
	}
	for ( ; cn_count > 0; cn_count--)
	{
		SendConfigureNotify(
			fw, fw->frame_g.x, fw->frame_g.y, fw->frame_g.width,
			fw->frame_g.height, 0, True);
	}
	if (do_send_event)
	{
		XFlush(dpy);
	}
#endif

	return;
}

void HandleDestroyNotify(const evh_args_t *ea)
{
	DBUG("HandleDestroyNotify","Routine Entered");

	destroy_window(ea->exc->w.fw);
	EWMH_ManageKdeSysTray(
		ea->exc->x.etrigger->xdestroywindow.window,
		ea->exc->x.etrigger->type);
	EWMH_WindowDestroyed();
	GNOME_SetClientList();

	return;
}

#define DEBUG_ENTERNOTIFY 0
#if DEBUG_ENTERNOTIFY
static int ecount=0;
#define ENTER_DBG(x) fprintf x;
#else
#define ENTER_DBG(x)
#endif
void HandleEnterNotify(const evh_args_t *ea)
{
	const XEnterWindowEvent *ewp;
	XEvent d;
	FvwmWindow *sf;
	static Bool is_initial_ungrab_pending = True;
	Bool is_tear_off_menu;
	const XEvent *te = ea->exc->x.etrigger;
	FvwmWindow * const fw = ea->exc->w.fw;

	DBUG("HandleEnterNotify","Routine Entered");
	ewp = &te->xcrossing;
ENTER_DBG((stderr, "++++++++ en (%d): fw 0x%08x w 0x%08x sw 0x%08xmode 0x%x detail 0x%x '%s'\n", ++ecount, (int)fw, (int)ewp->window, (int)ewp->subwindow, ewp->mode, ewp->detail, fw?fw->visible_name:"(none)"));

	if (ewp->window == Scr.Root && ewp->subwindow == None &&
	    ewp->detail == NotifyInferior && ewp->mode == NotifyNormal)
	{
		BroadcastPacket(MX_ENTER_WINDOW, 3, Scr.Root, NULL, NULL);
	}
	if (Scr.ColormapFocus == COLORMAP_FOLLOWS_MOUSE)
	{
		if (fw && !IS_ICONIFIED(fw) && ewp->window == FW_W(fw))
		{
			InstallWindowColormaps(fw);
		}
		else
		{
			/* make sure its for one of our windows */
			/* handle a subwindow cmap */
			InstallWindowColormaps(NULL);
		}
	}
	else if (!fw)
	{
		EnterSubWindowColormap(ewp->window);
	}
	if (Scr.flags.is_wire_frame_displayed)
	{
ENTER_DBG((stderr, "en: exit: iwfd\n"));
		/* Ignore EnterNotify events while a window is resized or moved
		 * as a wire frame; otherwise the window list may be screwed
		 * up. */
		return;
	}
	if (fw)
	{
		if (ewp->window != FW_W_FRAME(fw) &&
		    ewp->window != FW_W_PARENT(fw) &&
		    ewp->window != FW_W(fw) &&
		    ewp->window != FW_W_ICON_TITLE(fw) &&
		    ewp->window != FW_W_ICON_PIXMAP(fw))
		{
			/* Ignore EnterNotify that received by any of the sub
			 * windows that don't handle this event.  unclutter
			 * triggers these events sometimes, re focusing an
			 * unfocused window under the pointer */
ENTER_DBG((stderr, "en: exit: funny window\n"));
			return;
		}
	}
	if (Scr.focus_in_pending_window != NULL)
	{
ENTER_DBG((stderr, "en: exit: fipw\n"));
		/* Ignore EnterNotify event while we are waiting for a window to
		 * receive focus via Focus or FlipFocus commands. */
		focus_grab_buttons(fw);
		return;
	}
	if (ewp->mode == NotifyGrab)
	{
ENTER_DBG((stderr, "en: exit: NotifyGrab\n"));
		return;
	}
	else if (ewp->mode == NotifyNormal)
	{
ENTER_DBG((stderr, "en: NotifyNormal\n"));
		if (ewp->detail == NotifyNonlinearVirtual &&
		    ewp->focus == False && ewp->subwindow != None)
		{
			/* This takes care of some buggy apps that forget that
			 * one of their dialog subwindows has the focus after
			 * popping up a selection list several times (ddd,
			 * netscape). I'm not convinced that this does not
			 * break something else. */
ENTER_DBG((stderr, "en: NN: refreshing focus\n"));
			refresh_focus(fw);
		}
	}
	else if (ewp->mode == NotifyUngrab)
	{
ENTER_DBG((stderr, "en: NotifyUngrab\n"));
		/* Ignore events generated by grabbing or ungrabbing the
		 * pointer.  However, there is no way to prevent the client
		 * application from handling this event and, for example,
		 * grabbing the focus.  This will interfere with functions that
		 * transferred the focus to a different window. */
		if (is_initial_ungrab_pending)
		{
ENTER_DBG((stderr, "en: NU: initial ungrab pending (lgw = NULL)\n"));
			is_initial_ungrab_pending = False;
			xcrossing_last_grab_window = NULL;
		}
		else
		{
			if (ewp->detail == NotifyNonlinearVirtual &&
			    ewp->focus == False && ewp->subwindow != None)
			{
				/* see comment above */
ENTER_DBG((stderr, "en: NU: refreshing focus\n"));
				refresh_focus(fw);
			}
			if (fw && fw == xcrossing_last_grab_window)
			{
ENTER_DBG((stderr, "en: exit: NU: is last grab window\n"));
				if (ewp->window == FW_W_FRAME(fw) ||
				    ewp->window == FW_W_ICON_TITLE(fw) ||
				    ewp->window == FW_W_ICON_PIXMAP(fw))
				{
ENTER_DBG((stderr, "en: exit: NU: last grab window = NULL\n"));
					xcrossing_last_grab_window = NULL;
				}
				focus_grab_buttons(fw);

				return;
			}
			else if (fw)
			{
				if (ewp->window != FW_W_FRAME(fw) &&
				    ewp->window != FW_W_ICON_TITLE(fw) &&
				    ewp->window != FW_W_ICON_PIXMAP(fw))
				{
ENTER_DBG((stderr, "en: exit: NU: not frame window\n"));
					focus_grab_buttons(fw);
					return;
				}
			}
		}
	}
	if (fw)
	{
		is_initial_ungrab_pending = False;
	}

	/* look for a matching leaveNotify which would nullify this EnterNotify
	 */
	/*
	 * RBW - if we're in startup, this is a coerced focus, so we don't
	 * want to save the event time, or exit prematurely.
	 *
	 * Ignore LeaveNotify events for tear out menus - handled by menu code
	 */
	is_tear_off_menu =
		(fw && IS_TEAR_OFF_MENU(fw) && ewp->window == FW_W(fw));
	if (!fFvwmInStartup && !is_tear_off_menu &&
	    FCheckTypedWindowEvent(dpy, ewp->window, LeaveNotify, &d))
	{
		if (d.xcrossing.mode == NotifyNormal &&
		    d.xcrossing.detail != NotifyInferior)
		{
ENTER_DBG((stderr, "en: exit: found LeaveNotify\n"));
			return;
		}
	}

	if (ewp->window == Scr.Root)
	{
		FvwmWindow *lf = get_last_screen_focus_window();

		if (!Scr.flags.is_pointer_on_this_screen)
		{
			Scr.flags.is_pointer_on_this_screen = 1;
			if (lf && lf != &Scr.FvwmRoot &&
			    !FP_DO_UNFOCUS_LEAVE(FW_FOCUS_POLICY(lf)))
			{
				SetFocusWindow(lf, True, FOCUS_SET_FORCE);
			}
			else if (lf != &Scr.FvwmRoot)
			{
				ForceDeleteFocus();
			}
			else
			{
				/* This was the first EnterNotify event for the
				 * root window - ignore */
			}
			set_last_screen_focus_window(NULL);
		}
		else if (!(sf = get_focus_window()) ||
			 FP_DO_UNFOCUS_LEAVE(FW_FOCUS_POLICY(sf)))
		{
			DeleteFocus(True);
		}
		if (Scr.ColormapFocus == COLORMAP_FOLLOWS_MOUSE)
		{
			InstallWindowColormaps(NULL);
		}
		focus_grab_buttons(lf);
		return;
	}
	else
	{
		Scr.flags.is_pointer_on_this_screen = 1;
	}

	/* An EnterEvent in one of the PanFrameWindows activates the Paging or
	   an EdgeCommand. */
	if (is_pan_frame(ewp->window))
	{
		char *edge_command = NULL;

		/* check for edge commands */
		if (ewp->window == Scr.PanFrameTop.win)
		{
			edge_command = Scr.PanFrameTop.command;
		}
		else if (ewp->window == Scr.PanFrameBottom.win)
		{
			edge_command = Scr.PanFrameBottom.command;
		}
		else if (ewp->window == Scr.PanFrameLeft.win)
		{
			edge_command = Scr.PanFrameLeft.command;
		}
		else if (ewp->window == Scr.PanFrameRight.win)
		{
			edge_command = Scr.PanFrameRight.command;
		}
		if (edge_command)
		{
			execute_function(NULL, ea->exc, edge_command, 0);
		}
		else
		{
			/* no edge command for this pan frame - so we do
			 * HandlePaging */
			int delta_x = 0;
			int delta_y = 0;
			XEvent e;

			/* this was in the HandleMotionNotify before, HEDU */
			Scr.flags.is_pointer_on_this_screen = 1;
			e = *te;
			HandlePaging(
				&e, Scr.EdgeScrollX, Scr.EdgeScrollY, &JunkX,
				&JunkY, &delta_x, &delta_y, True, True, False);
			return;
		}
	}
	if (!fw)
	{
		return;
	}
	if (IS_EWMH_DESKTOP(FW_W(fw)))
	{
		BroadcastPacket(MX_ENTER_WINDOW, 3, Scr.Root, NULL, NULL);
		return;
	}
	if (ewp->window == FW_W_FRAME(fw) ||
	    ewp->window == FW_W_ICON_TITLE(fw) ||
	    ewp->window == FW_W_ICON_PIXMAP(fw))
	{
		BroadcastPacket(
			MX_ENTER_WINDOW, 3, FW_W(fw), FW_W_FRAME(fw),
			(unsigned long)fw);
	}
	sf = get_focus_window();
	if (sf && fw != sf && FP_DO_UNFOCUS_LEAVE(FW_FOCUS_POLICY(sf)))
	{
ENTER_DBG((stderr, "en: delete focus\n"));
		DeleteFocus(True);
	}
	focus_grab_buttons(fw);
	if (FP_DO_FOCUS_ENTER(FW_FOCUS_POLICY(fw)))
	{
ENTER_DBG((stderr, "en: set mousey focus\n"));
                if (ewp->window == FW_W(fw))
                {
                  /*  Event is for the client window...*/
#ifndef EXPERIMENTAL_ROU_HANDLING_V2
/*  RBW --  This may still be needed at times, I'm not sure yet.  */
		  SetFocusWindowClientEntered(fw, True, FOCUS_SET_BY_ENTER);
#else
		  SetFocusWindow(fw, True, FOCUS_SET_BY_ENTER);
#endif
                }
                else
                {
                  /*  Event is for the frame...*/
		  SetFocusWindow(fw, True, FOCUS_SET_BY_ENTER);
                }
	}
	else if (focus_is_focused(fw) && focus_does_accept_input_focus(fw))
	{
		/* We have to refresh the focus window here in case we left the
		 * focused fvwm window.  Motif apps may lose the input focus
		 * otherwise.  But do not try to refresh the focus of
		 * applications that want to handle it themselves. */
		focus_force_refresh_focus(fw);
	}
	else if (sf != fw)
	{
		/* Give the window a chance to grab the buttons needed for
		 * raise-on-click */
		focus_grab_buttons(sf);
	}

	/* We get an EnterNotify with mode == UnGrab when fvwm releases the
	 * grab held during iconification. We have to ignore this, or icon
	 * title will be initially raised. */
	if (IS_ICONIFIED(fw) && (ewp->mode == NotifyNormal) &&
	    (ewp->window == FW_W_ICON_PIXMAP(fw) ||
	     ewp->window == FW_W_ICON_TITLE(fw)) &&
	    FW_W_ICON_PIXMAP(fw) != None)
	{
		SET_ICON_ENTERED(fw, 1);
		DrawIconWindow(fw, True, False, False, NULL);
	}
	/* Check for tear off menus */
	if (is_tear_off_menu)
	{
		menu_enter_tear_off_menu(ea->exc);
	}

	return;
}

void HandleExpose(const evh_args_t *ea)
{
	XEvent e;
	FvwmWindow * const fw = ea->exc->w.fw;

	e = *ea->exc->x.etrigger;
#if 0
	/* This doesn't work well. Sometimes, the expose count is zero although
	 * dozens of expose events are pending.  This happens all the time
	 * during a shading animation.  Simply flush expose events
	 * unconditionally. */
	if (e.xexpose.count != 0)
	{
		flush_accumulate_expose(e.xexpose.window, &e);
	}
#else
	flush_accumulate_expose(e.xexpose.window, &e);
#endif
	if (fw == NULL)
	{
		return;
	}
	if (e.xany.window == FW_W_ICON_TITLE(fw) ||
	    e.xany.window == FW_W_ICON_PIXMAP(fw))
	{
		DrawIconWindow(fw, True, True, False, &e);
		return;
	}
	else if (IS_TEAR_OFF_MENU(fw) && e.xany.window == FW_W(fw))
	{
		/* refresh the contents of the torn out menu */
		menu_expose(&e, NULL);
	}

	return;
}

void HandleFocusIn(const evh_args_t *ea)
{
	XEvent d;
	Window w = None;
	Window focus_w = None;
	Window focus_fw = None;
	Pixel fc = 0;
	Pixel bc = 0;
	FvwmWindow *ffw_old = get_focus_window();
	FvwmWindow *sf;
	Bool do_force_broadcast = False;
	Bool is_unmanaged_focused = False;
	static Window last_focus_w = None;
	static Window last_focus_fw = None;
	static Bool was_nothing_ever_focused = True;
	FvwmWindow *fw = ea->exc->w.fw;

	DBUG("HandleFocusIn","Routine Entered");

	Scr.focus_in_pending_window = NULL;
	/* This is a hack to make the PointerKey command work */
	if (ea->exc->x.etrigger->xfocus.detail != NotifyPointer)
	{
		/**/
		w = ea->exc->x.etrigger->xany.window;
	}
	while (FCheckTypedEvent(dpy, FocusIn, &d))
	{
		/* dito */
		if (d.xfocus.detail != NotifyPointer)
		{
			/**/
			w = d.xany.window;
		}
	}
	/* dito */
	if (w == None)
	{
		return;
	}
	/**/
	if (XFindContext(dpy, w, FvwmContext, (caddr_t *) &fw) == XCNOENT)
	{
		fw = NULL;
	}

	Scr.UnknownWinFocused = None;
	if (!fw)
	{
		if (w != Scr.NoFocusWin)
		{
			Scr.UnknownWinFocused = w;
			Scr.StolenFocusWin =
				(ffw_old != NULL) ? FW_W(ffw_old) : None;
			focus_w = w;
			is_unmanaged_focused = True;
		}
		else
		{
			border_draw_decorations(
				Scr.Hilite, PART_ALL, False, True, CLEAR_ALL,
				NULL, NULL);
			if (Scr.ColormapFocus == COLORMAP_FOLLOWS_FOCUS)
			{
				if ((Scr.Hilite)&&(!IS_ICONIFIED(Scr.Hilite)))
				{
					InstallWindowColormaps(Scr.Hilite);
				}
				else
				{
					InstallWindowColormaps(NULL);
				}
			}
		}
		/* Not very useful if no window that fvwm and its modules know
		 * about has the focus. */
		fc = GetColor(DEFAULT_FORE_COLOR);
		bc = GetColor(DEFAULT_BACK_COLOR);
	}
	else if (fw != Scr.Hilite ||
		 /* domivogt (16-May-2000): This check is necessary to force
		  * sending a M_FOCUS_CHANGE packet after an unmanaged window
		  * was focused. Otherwise fvwm would believe that Scr.Hilite
		  * was still focused and not send any info to the modules. */
		 last_focus_fw == None ||
		 IS_FOCUS_CHANGE_BROADCAST_PENDING(fw))
	{
		do_force_broadcast = IS_FOCUS_CHANGE_BROADCAST_PENDING(fw);
		SET_FOCUS_CHANGE_BROADCAST_PENDING(fw, 0);
		if (fw != Scr.Hilite)
		{
			border_draw_decorations(
				fw, PART_ALL, True, True, CLEAR_ALL, NULL,
				NULL);
		}
		focus_w = FW_W(fw);
		focus_fw = FW_W_FRAME(fw);
		fc = fw->hicolors.fore;
		bc = fw->hicolors.back;
		set_focus_window(fw);
		if (Scr.ColormapFocus == COLORMAP_FOLLOWS_FOCUS)
		{
			if ((Scr.Hilite)&&(!IS_ICONIFIED(Scr.Hilite)))
			{
				InstallWindowColormaps(Scr.Hilite);
			}
			else
			{
				InstallWindowColormaps(NULL);
			}
		}
	}
	else
	{
		return;
	}
	if (was_nothing_ever_focused || last_focus_fw == None ||
	    focus_w != last_focus_w || focus_fw != last_focus_fw ||
	    do_force_broadcast)
	{
		if (!Scr.bo.FlickeringQtDialogsWorkaround ||
		    !is_unmanaged_focused)
		{
			BroadcastPacket(
				M_FOCUS_CHANGE, 5, focus_w, focus_fw,
				(unsigned long)IsLastFocusSetByMouse(), fc, bc);
			EWMH_SetActiveWindow(focus_w);
		}
		last_focus_w = focus_w;
		last_focus_fw = focus_fw;
		was_nothing_ever_focused = False;
	}
	if ((sf = get_focus_window()) != ffw_old)
	{
		focus_grab_buttons(sf);
		focus_grab_buttons(ffw_old);
	}

	return;
}

void HandleFocusOut(const evh_args_t *ea)
{
	if (Scr.UnknownWinFocused != None && Scr.StolenFocusWin != None &&
	    ea->exc->x.etrigger->xfocus.window == Scr.UnknownWinFocused)
	{
		FOCUS_SET(Scr.StolenFocusWin);
		Scr.UnknownWinFocused = None;
		Scr.StolenFocusWin = None;
	}

	return;
}

void HandleKeyPress(const evh_args_t *ea)
{
	char *action;
	FvwmWindow *sf;
	KeyCode kc;
	int context2;
	const XEvent *te = ea->exc->x.etrigger;
	const FvwmWindow * const fw = ea->exc->w.fw;
	Bool is_second_binding;

	DBUG("HandleKeyPress","Routine Entered");

	PressedW = None;

	/* Here's a real hack - some systems have two keys with the
	 * same keysym and different keycodes. This converts all
	 * the cases to one keycode. */
	kc = XKeysymToKeycode(
		dpy, XKeycodeToKeysym(dpy, te->xkey.keycode, 0));

	/* Check if there is something bound to the key */
	sf = get_focus_window();
	context2 = (sf != NULL && sf != ea->exc->w.fw) ?
		C_WINDOW : ea->exc->w.wcontext;
	action = CheckTwoBindings(
		&is_second_binding, Scr.AllBindings, STROKE_ARG(0) kc,
		te->xkey.state, GetUnusedModifiers(), ea->exc->w.wcontext,
		BIND_PKEYPRESS, context2, BIND_KEYPRESS);
	if (action != NULL)
	{
		const exec_context_t *exc;
		exec_context_changes_t ecc;

		exc = ea->exc;
		if (is_second_binding == True)
		{
			ecc.w.fw = sf;
			ecc.w.wcontext = C_WINDOW;
			exc = exc_clone_context(
				ea->exc, &ecc, ECC_FW | ECC_WCONTEXT);
		}
		execute_function(NULL, exc, action, 0);
		if (is_second_binding == True)
		{
			exc_destroy_context(exc);
		}
		return;
	}

	/* if we get here, no function key was bound to the key.  Send it
	 * to the client if it was in a window we know about. */
	sf = get_focus_window();
	if (sf && te->xkey.window != FW_W(sf))
	{
		XEvent e;

		e = *te;
		e.xkey.window = FW_W(sf);
		FSendEvent(dpy, FW_W(sf), False, KeyPressMask, &e);
	}
	else if (fw && te->xkey.window != FW_W(fw))
	{
		XEvent e;

		e = *te;
		e.xkey.window = FW_W(fw);
		FSendEvent(dpy, FW_W(fw), False, KeyPressMask, &e);
	}

	return;
}

void HandleLeaveNotify(const evh_args_t *ea)
{
	const XEvent *te = ea->exc->x.etrigger;
	FvwmWindow * const fw = ea->exc->w.fw;

	DBUG("HandleLeaveNotify","Routine Entered");

ENTER_DBG((stderr, "-------- ln (%d): fw 0x%08x w 0x%08x sw 0x%08x mode 0x%x detail 0x%x '%s'\n", ++ecount, (int)fw, (int)te->xcrossing.window, (int)te->xcrossing.subwindow, te->xcrossing.mode, te->xcrossing.detail, fw?fw->visible_name:"(none)"));
	/* Ignore LeaveNotify events while a window is resized or moved as a
	 * wire frame; otherwise the window list may be screwed up. */
	if (Scr.flags.is_wire_frame_displayed)
	{
		return;
	}
	if (te->xcrossing.mode != NotifyNormal)
	{
		/* Ignore events generated by grabbing or ungrabbing the
		 * pointer.  However, there is no way to prevent the client
		 * application from handling this event and, for example,
		 * grabbing the focus.  This will interfere with functions that
		 * transferred the focus to a different window.  It is
		 * necessary to check for LeaveNotify events on the client
		 * window too in case buttons are not grabbed on it. */
		if (te->xcrossing.mode == NotifyGrab && fw &&
		    (te->xcrossing.window == FW_W_FRAME(fw) ||
		     te->xcrossing.window == FW_W(fw) ||
		     te->xcrossing.window == FW_W_ICON_TITLE(fw) ||
		     te->xcrossing.window == FW_W_ICON_PIXMAP(fw)))
		{
ENTER_DBG((stderr, "ln: *** lgw = 0x%08x\n", (int)fw));
			xcrossing_last_grab_window = fw;
		}
#ifdef FOCUS_EXPANDS_TITLE
		if (fw && IS_ICONIFIED(fw))
		{
			SET_ICON_ENTERED(fw, 0);
			DrawIconWindow(fw, True, False, False, NULL);
		}
#endif
		return;
	}
	/* CDE-like behaviour of raising the icon title if the icon
	   gets the focus (in particular if the cursor is over the icon) */
	if (fw && IS_ICONIFIED(fw))
	{
		SET_ICON_ENTERED(fw,0);
		DrawIconWindow(fw, True, False, False, NULL);
	}

	/* If we leave the root window, then we're really moving
	 * another screen on a multiple screen display, and we
	 * need to de-focus and unhighlight to make sure that we
	 * don't end up with more than one highlighted window at a time */
	if (te->xcrossing.window == Scr.Root &&
	   /* domivogt (16-May-2000): added this test because somehow fvwm
	    * sometimes gets a LeaveNotify on the root window although it is
	    * single screen. */
	    Scr.NumberOfScreens > 1)
	{
		if (te->xcrossing.mode == NotifyNormal)
		{
			if (te->xcrossing.detail != NotifyInferior)
			{
				FvwmWindow *sf = get_focus_window();

				Scr.flags.is_pointer_on_this_screen = 0;
				set_last_screen_focus_window(sf);
				if (sf != NULL)
				{
					DeleteFocus(True);
				}
				if (Scr.Hilite != NULL)
				{
					border_draw_decorations(
						Scr.Hilite, PART_ALL, False,
						True, CLEAR_ALL, NULL, NULL);
				}
			}
		}
	}
	else
	{
		/* handle a subwindow cmap */
		LeaveSubWindowColormap(te->xany.window);
	}
	if (fw != NULL &&
	    (te->xcrossing.window == FW_W_FRAME(fw) ||
	     te->xcrossing.window == FW_W_ICON_TITLE(fw) ||
	     te->xcrossing.window == FW_W_ICON_PIXMAP(fw)))
	{
		BroadcastPacket(
			MX_LEAVE_WINDOW, 3, FW_W(fw), FW_W_FRAME(fw),
			(unsigned long)fw);
}

	return;
}

void HandleMapNotify(const evh_args_t *ea)
{
	Bool is_on_this_page = False;
	const XEvent *te = ea->exc->x.etrigger;
	FvwmWindow * const fw = ea->exc->w.fw;

	DBUG("HandleMapNotify","Routine Entered");

	if (!fw)
	{
		if (te->xmap.override_redirect == True &&
		    te->xmap.window != Scr.NoFocusWin)
		{
			XSelectInput(dpy, te->xmap.window, XEVMASK_ORW);
			Scr.UnknownWinFocused = te->xmap.window;
		}
		return;
	}
	if (te->xmap.window == FW_W_FRAME(fw))
	{
		/* Now that we know the frame is mapped after capturing the
		 * window we do not need StructureNotifyMask events anymore. */
		XSelectInput(dpy, FW_W_FRAME(fw), XEVMASK_FRAMEW);
	}
	/* Except for identifying over-ride redirect window mappings, we
	 * don't need or want windows associated with the
	 * SubstructureNotifyMask */
	if (te->xmap.event != te->xmap.window)
	{
		return;
	}
	SET_MAP_PENDING(fw, 0);
	/* don't map if the event was caused by a de-iconify */
	if (IS_ICONIFY_PENDING(fw))
	{
		return;
	}

	/* Make sure at least part of window is on this page before giving it
	 * focus... */
	is_on_this_page = IsRectangleOnThisPage(&(fw->frame_g), fw->Desk);

	/*
	 * Need to do the grab to avoid race condition of having server send
	 * MapNotify to client before the frame gets mapped; this is bad because
	 * the client would think that the window has a chance of being viewable
	 * when it really isn't.
	 */
	MyXGrabServer (dpy);
	if (FW_W_ICON_TITLE(fw))
	{
		XUnmapWindow(dpy, FW_W_ICON_TITLE(fw));
	}
	if (FW_W_ICON_PIXMAP(fw) != None)
	{
		XUnmapWindow(dpy, FW_W_ICON_PIXMAP(fw));
	}
	XMapSubwindows(dpy, FW_W_FRAME(fw));
	if (fw->Desk == Scr.CurrentDesk)
	{
		XMapWindow(dpy, FW_W_FRAME(fw));
	}
	if (IS_ICONIFIED(fw))
	{
		BroadcastPacket(
			M_DEICONIFY, 3, FW_W(fw), FW_W_FRAME(fw),
			(unsigned long)fw);
	}
	else
	{
		BroadcastPacket(
			M_MAP, 3, FW_W(fw), FW_W_FRAME(fw), (unsigned long)fw);
	}

	if (is_on_this_page &&
	    focus_query_open_grab_focus(fw, get_focus_window()) == True)
	{
		SetFocusWindow(fw, True, FOCUS_SET_FORCE);
	}
	border_draw_decorations(
		fw, PART_ALL, (fw == get_focus_window()) ? True : False, True,
		CLEAR_ALL, NULL, NULL);
	MyXUngrabServer (dpy);
	SET_MAPPED(fw, 1);
	SET_ICONIFIED(fw, 0);
	SET_ICON_UNMAPPED(fw, 0);
	if (DO_ICONIFY_AFTER_MAP(fw))
	{
		initial_window_options_type win_opts;

		/* finally, if iconification was requested before the window
		 * was mapped, request it now. */
		memset(&win_opts, 0, sizeof(win_opts));
		Iconify(fw, &win_opts);
		SET_ICONIFY_AFTER_MAP(fw, 0);
	}
	focus_grab_buttons_on_layer(fw->layer);

	return;
}

void HandleMappingNotify(const evh_args_t *ea)
{
	XRefreshKeyboardMapping(&ea->exc->x.etrigger->xmapping);

	return;
}

void HandleMapRequest(const evh_args_t *ea)
{
	DBUG("HandleMapRequest","Routine Entered");

	if (fFvwmInStartup)
	{
		/* Just map the damn thing, decorations are added later
		 * in CaptureAllWindows. */
		XMapWindow(dpy, ea->exc->x.etrigger->xmaprequest.window);
		return;
	}
	HandleMapRequestKeepRaised(ea, None, NULL, NULL);

	return;
}

void HandleMapRequestKeepRaised(
	const evh_args_t *ea, Window KeepRaised, FvwmWindow *ReuseWin,
	initial_window_options_type *win_opts)
{
	Bool is_on_this_page = False;
	Bool is_new_window = False;
	FvwmWindow *tmp;
	FvwmWindow *sf;
	initial_window_options_type win_opts_bak;
	Window ew;
	FvwmWindow *fw;

	if (win_opts == NULL)
	{
		memset(&win_opts_bak, 0, sizeof(win_opts_bak));
		win_opts = &win_opts_bak;
	}
	ew = ea->exc->w.w;
	if (ReuseWin == NULL)
	{
		if (XFindContext(dpy, ew, FvwmContext, (caddr_t *)&fw) ==
		    XCNOENT)
		{
			fw = NULL;
		}
		else if (IS_MAP_PENDING(fw))
		{
			/* The window is already going to be mapped, no need to
			 * do that twice */
			return;
		}
	}
	else
	{
		fw = ReuseWin;
	}

	if (fw == NULL && EWMH_IsKdeSysTrayWindow(ew))
	{
		/* This means that the window is swallowed by kicker and that
		 * kicker restart or exit. As we should assume that kicker
		 * restart we should return here, if not we go into trouble
		 * ... */
		return;
	}
	if (!win_opts->flags.do_override_ppos)
	{
		XFlush(dpy);
	}

	/* If the window has never been mapped before ... */
	if (!fw || (fw && DO_REUSE_DESTROYED(fw)))
	{
		/* Add decorations. */
		fw = AddWindow(ea->exc, ReuseWin, win_opts);
		if (fw == NULL)
		{
			return;
		}
		is_new_window = True;
	}
	/*
	 * Make sure at least part of window is on this page
	 * before giving it focus...
	 */
	is_on_this_page = IsRectangleOnThisPage(&(fw->frame_g), fw->Desk);
	if (KeepRaised != None)
	{
		XRaiseWindow(dpy, KeepRaised);
	}
	/* If it's not merely iconified, and we have hints, use them. */

	if (IS_ICONIFIED(fw))
	{
		/* If no hints, or currently an icon, just "deiconify" */
		DeIconify(fw);
	}
	else if (IS_MAPPED(fw))
	{
		/* the window is already mapped - fake a MapNotify event */
		fake_map_unmap_notify(fw, MapNotify);
	}
	else
	{
		int state;

		if (fw->wmhints && (fw->wmhints->flags & StateHint))
		{
			state = fw->wmhints->initial_state;
		}
		else
		{
			state = NormalState;
		}
		if (win_opts->initial_state != DontCareState)
		{
			state = win_opts->initial_state;
		}

		switch (state)
		{
		case DontCareState:
		case NormalState:
		case InactiveState:
		default:
			MyXGrabServer(dpy);
			if (fw->Desk == Scr.CurrentDesk)
			{
				Bool do_grab_focus;

				SET_MAP_PENDING(fw, 1);
				XMapWindow(dpy, FW_W_FRAME(fw));
				XMapWindow(dpy, FW_W(fw));
				SetMapStateProp(fw, NormalState);
				if (Scr.flags.is_map_desk_in_progress)
				{
					do_grab_focus = False;
				}
				else if (!is_on_this_page)
				{
					do_grab_focus = False;
				}
				else if (focus_query_open_grab_focus(
						 fw, get_focus_window()) ==
					 True)
				{
					do_grab_focus = True;
				}
				else
				{
					do_grab_focus = False;
				}
				if (do_grab_focus)
				{
					SetFocusWindow(
						fw, True, FOCUS_SET_FORCE);
				}
				else
				{
					/* make sure the old focused window
					 * still has grabbed all necessary
					 * buttons. */
					focus_grab_buttons(
						get_focus_window());
				}
			}
			else
			{
#ifndef ICCCM2_UNMAP_WINDOW_PATCH
				/* nope, this is forbidden by the ICCCM2 */
				XMapWindow(dpy, FW_W(fw));
				SetMapStateProp(fw, NormalState);
#else
				/* Since we will not get a MapNotify, set the
				 * IS_MAPPED flag manually. */
				SET_MAPPED(fw, 1);
				SetMapStateProp(fw, IconicState);
				/* fake that the window was mapped to allow
				 * modules to swallow it */
				BroadcastPacket(
					M_MAP, 3, FW_W(fw),FW_W_FRAME(fw),
					(unsigned long)fw);
#endif
			}
			MyXUngrabServer(dpy);
			break;

		case IconicState:
			if (is_new_window)
			{
				/* the window will not be mapped - fake a
				 * MapNotify and an UnmapNotify event.  Can't
				 * remember exactly why this is necessary, but
				 * probably something w/ (de)iconify state
				 * confusion. */
				fake_map_unmap_notify(fw, MapNotify);
				fake_map_unmap_notify(fw, UnmapNotify);
			}
			if (win_opts->flags.is_iconified_by_parent ||
			    ((tmp = get_transientfor_fvwmwindow(fw)) &&
			     IS_ICONIFIED(tmp)))
			{
				win_opts->flags.is_iconified_by_parent = 0;
				SET_ICONIFIED_BY_PARENT(fw, 1);
			}
			if (USE_ICON_POSITION_HINT(fw) && fw->wmhints &&
			    (fw->wmhints->flags & IconPositionHint))
			{
				win_opts->default_icon_x = fw->wmhints->icon_x;
				win_opts->default_icon_y = fw->wmhints->icon_y;
			}
			Iconify(fw, win_opts);
			break;
		}
	}
	if (IS_SHADED(fw))
	{
		BroadcastPacket(M_WINDOWSHADE, 3, FW_W(fw), FW_W_FRAME(fw),
				(unsigned long)fw);
	}
	/* If the newly mapped window overlaps the focused window, make sure
	 * ClickToFocusRaises and MouseFocusClickRaises work again. */
	sf = get_focus_window();
	if (sf != NULL)
	{
		focus_grab_buttons(sf);
	}
	if (win_opts->flags.is_menu)
	{
		SET_MAPPED(fw, 1);
		SET_MAP_PENDING(fw, 0);
	}
	EWMH_SetClientList();
	GNOME_SetClientList();

	return;
}

#ifdef HAVE_STROKE
void HandleMotionNotify(const evh_args_t *ea)
{
	DBUG("HandleMotionNotify","Routine Entered");

	if (send_motion == True)
	{
		stroke_record(
			ea->exc->x.etrigger->xmotion.x,
			ea->exc->x.etrigger->xmotion.y);
	}

	return;
}
#endif /* HAVE_STROKE */

void HandlePropertyNotify(const evh_args_t *ea)
{
	Bool OnThisPage = False;
	Bool has_icon_changed = False;
	Bool has_icon_pixmap_hint_changed = False;
	Bool has_icon_window_hint_changed = False;
	FlocaleNameString new_name = { NoName, NULL };
	int old_wmhints_flags;
	const XEvent *te = ea->exc->x.etrigger;
	char *urgency_action = NULL;
	FvwmWindow * const fw = ea->exc->w.fw;

	DBUG("HandlePropertyNotify","Routine Entered");

	if (te->xproperty.window == Scr.Root &&
	    te->xproperty.state == PropertyNewValue &&
	    (te->xproperty.atom == _XA_XSETROOT_ID ||
	     te->xproperty.atom == _XA_XROOTPMAP_ID))
	{
		/* background change */
		/* _XA_XSETROOT_ID is used by fvwm-root, xli and more (xv sends
		 * no property  notify?).  _XA_XROOTPMAP_ID is used by Esetroot
		 * compatible program: the problem here is that with some
		 * Esetroot compatible program we get the message _before_ the
		 * background change. This is fixed with Esetroot 9.2 (not yet
		 * released, 2002-01-14) */

		/* update icon window with some alpha */
		FvwmWindow *t;
		int cs;

		for (t = Scr.FvwmRoot.next; t != NULL; t = t->next)
		{
			if (Scr.Hilite == t)
			{
				cs = t->cs_hi;
			}
			else
			{
				cs = t->cs;
			}
			if (IS_ICONIFIED(t) && !IS_ICON_SUPPRESSED(t) &&
			    (t->icon_alphaPixmap != None ||
			     (cs >= 0 &&
			      Colorset[cs].icon_alpha_percent < 100)))
			{
				DrawIconWindow(t, False, True, False, NULL);
			}
		}
		if (te->xproperty.atom == _XA_XROOTPMAP_ID)
		{
			update_root_transparent_colorset(te->xproperty.atom);
		}
		BroadcastPropertyChange(
			MX_PROPERTY_CHANGE_BACKGROUND, 0, 0, "");
		return;
	}

	if (!fw)
	{
		return;
	}
	if (XGetGeometry(
		    dpy, FW_W(fw), &JunkRoot, &JunkX, &JunkY, &JunkWidth,
		    &JunkHeight, &JunkBW, &JunkDepth) == 0)
	{
		return;
	}

	/*
	 * Make sure at least part of window is on this page
	 * before giving it focus...
	 */
	OnThisPage = IsRectangleOnThisPage(&(fw->frame_g), fw->Desk);

	switch (te->xproperty.atom)
	{
	case XA_WM_TRANSIENT_FOR:
		flush_property_notify(XA_WM_TRANSIENT_FOR, FW_W(fw));
		if (setup_transientfor(fw) == True)
		{
			RaiseWindow(fw);
		}
		break;

	case XA_WM_NAME:
		flush_property_notify(XA_WM_NAME, FW_W(fw));
		if (HAS_EWMH_WM_NAME(fw))
		{
			return;
		}
		FlocaleGetNameProperty(XGetWMName, dpy, FW_W(fw), &new_name);
		if (new_name.name == NULL)
		{
			return;
		}
		free_window_names (fw, True, False);
		fw->name = new_name;
		if (fw->name.name &&
		    strlen(fw->name.name) > MAX_WINDOW_NAME_LEN)
		{
			(fw->name.name)[MAX_WINDOW_NAME_LEN] = 0;
		}
		SET_NAME_CHANGED(fw, 1);
		if (fw->name.name == NULL)
		{
			fw->name.name = NoName; /* must not happen */
		}
		setup_visible_name(fw, False);
		BroadcastWindowIconNames(fw, True, False);
		/* fix the name in the title bar */
		if (!IS_ICONIFIED(fw))
		{
			border_draw_decorations(
				fw, PART_TITLE, (Scr.Hilite == fw), True,
				CLEAR_ALL, NULL, NULL);
		}
		EWMH_SetVisibleName(fw, False);
		/*
		 * if the icon name is NoName, set the name of the icon to be
		 * the same as the window
		 */
		if (!WAS_ICON_NAME_PROVIDED(fw))
		{
			fw->icon_name = fw->name;
			setup_visible_name(fw, True);
			BroadcastWindowIconNames(fw, False, True);
			RedoIconName(fw);
		}
		break;

	case XA_WM_ICON_NAME:
		flush_property_notify(XA_WM_ICON_NAME, FW_W(fw));
		if (HAS_EWMH_WM_ICON_NAME(fw))
		{
			return;
		}
		FlocaleGetNameProperty(
			XGetWMIconName, dpy, FW_W(fw), &new_name);
		if (new_name.name == NULL)
		{
			return;
		}
		free_window_names(fw, False, True);
		fw->icon_name = new_name;
		if (fw->icon_name.name && strlen(fw->icon_name.name) >
		    MAX_ICON_NAME_LEN)
		{
			/* limit to prevent hanging X server */
			(fw->icon_name.name)[MAX_ICON_NAME_LEN] = 0;
		}
		SET_WAS_ICON_NAME_PROVIDED(fw, 1);
		if (fw->icon_name.name == NULL)
		{
			fw->icon_name .name= fw->name.name;
			SET_WAS_ICON_NAME_PROVIDED(fw, 0);
		}
		setup_visible_name(fw, True);
		BroadcastWindowIconNames(fw, False, True);
		RedoIconName(fw);
		EWMH_SetVisibleName(fw, True);
		break;

	case XA_WM_HINTS:
		flush_property_notify(XA_WM_HINTS, FW_W(fw));
		/* clasen@mathematik.uni-freiburg.de - 02/01/1998 - new -
		   the urgency flag is an ICCCM 2.0 addition to the WM_HINTS. */
		old_wmhints_flags = 0;
		if (fw->wmhints)
		{
			old_wmhints_flags = fw->wmhints->flags;
			XFree ((char *) fw->wmhints);
		}
		setup_wm_hints(fw);
		if (fw->wmhints == NULL)
		{
			return;
		}

		/*
		 * rebuild icon if the client either provides an icon
		 * pixmap or window or has reset the hints to `no icon'.
		 */
		if ((fw->wmhints->flags & IconPixmapHint) ||
		    (old_wmhints_flags & IconPixmapHint))
		{
ICON_DBG((stderr,"hpn: iph changed (%d) '%s'\n", !!(int)(fw->wmhints->flags & IconPixmapHint), fw->name));
			has_icon_pixmap_hint_changed = True;
		}
		if ((fw->wmhints->flags & IconWindowHint) ||
		    (old_wmhints_flags & IconWindowHint))
		{
ICON_DBG((stderr,"hpn: iwh changed (%d) '%s'\n", !!(int)(fw->wmhints->flags & IconWindowHint), fw->name));
			has_icon_window_hint_changed = True;
			SET_USE_EWMH_ICON(fw, False);
		}
		increase_icon_hint_count(fw);
		if (has_icon_window_hint_changed ||
		    has_icon_pixmap_hint_changed)
		{
			if (ICON_OVERRIDE_MODE(fw) == ICON_OVERRIDE)
			{
ICON_DBG((stderr,"hpn: icon override '%s'\n", fw->name));
				has_icon_changed = False;
			}
			else if (ICON_OVERRIDE_MODE(fw) ==
				 NO_ACTIVE_ICON_OVERRIDE)
			{
				if (has_icon_pixmap_hint_changed)
				{
					if (WAS_ICON_HINT_PROVIDED(fw) ==
					    ICON_HINT_MULTIPLE)
					{
ICON_DBG((stderr,"hpn: using further iph '%s'\n", fw->name));
						has_icon_changed = True;
					}
					else  if (fw->icon_bitmap_file ==
						  NULL ||
						  fw->icon_bitmap_file ==
						  Scr.DefaultIcon)
					{
ICON_DBG((stderr,"hpn: using first iph '%s'\n", fw->name));
						has_icon_changed = True;
					}
					else
					{
						/* ignore the first icon pixmap
						 * hint if the application did
						 * not provide it from the
						 * start */
ICON_DBG((stderr,"hpn: first iph ignored '%s'\n", fw->name));
						has_icon_changed = False;
					}
				}
				else if (has_icon_window_hint_changed)
				{
ICON_DBG((stderr,"hpn: using iwh '%s'\n", fw->name));
					has_icon_changed = True;
				}
				else
				{
ICON_DBG((stderr,"hpn: iwh not changed, hint ignored '%s'\n", fw->name));
					has_icon_changed = False;
				}
			}
			else /* NO_ICON_OVERRIDE */
			{
ICON_DBG((stderr,"hpn: using hint '%s'\n", fw->name));
				has_icon_changed = True;
			}

			if (USE_EWMH_ICON(fw))
			{
				has_icon_changed = False;
			}

			if (has_icon_changed)
			{
ICON_DBG((stderr,"hpn: icon changed '%s'\n", fw->name));
				/* Okay, the icon hint has changed and style
				 * options tell us to honour this change.  Now
				 * let's see if we have to use the application
				 * provided pixmap or window (if any), the icon
				 * file provided by the window's style or the
				 * default style's icon. */
				if (fw->icon_bitmap_file == Scr.DefaultIcon)
				{
					fw->icon_bitmap_file = NULL;
				}
				if (!fw->icon_bitmap_file &&
				    !(fw->wmhints->flags &
				      (IconPixmapHint|IconWindowHint)))
				{
					fw->icon_bitmap_file =
						(Scr.DefaultIcon) ?
						Scr.DefaultIcon : NULL;
				}
				fw->iconPixmap = (Window)NULL;
				ChangeIconPixmap(fw);
			}
		}

		/* clasen@mathematik.uni-freiburg.de - 02/01/1998 - new -
		 * the urgency flag is an ICCCM 2.0 addition to the WM_HINTS.
		 * Treat urgency changes by calling user-settable functions.
		 * These could e.g. deiconify and raise the window or
		 * temporarily change the decor. */
		if (!(old_wmhints_flags & XUrgencyHint) &&
		    (fw->wmhints->flags & XUrgencyHint))
		{
			urgency_action = "Function UrgencyFunc";
		}
		if ((old_wmhints_flags & XUrgencyHint) &&
		    !(fw->wmhints->flags & XUrgencyHint))
		{
			urgency_action = "Function UrgencyDoneFunc";
		}
		if (urgency_action)
		{
			const exec_context_t *exc;
			exec_context_changes_t ecc;

			ecc.w.fw = fw;
			ecc.w.wcontext = C_WINDOW;
			exc = exc_clone_context(
				ea->exc, &ecc, ECC_FW | ECC_WCONTEXT);
			execute_function(NULL, exc, urgency_action, 0);
			exc_destroy_context(exc);
		}
		break;
	case XA_WM_NORMAL_HINTS:
		/* just mark wm normal hints as changed and look them up when
		 * the next ConfigureRequest w/ x, y, width or height set
		 * arrives. */
		SET_HAS_NEW_WM_NORMAL_HINTS(fw, 1);
		break;
	default:
		if (te->xproperty.atom == _XA_WM_PROTOCOLS)
		{
			FetchWmProtocols (fw);
		}
		else if (te->xproperty.atom == _XA_WM_COLORMAP_WINDOWS)
		{
			FetchWmColormapWindows (fw);    /* frees old data */
			ReInstallActiveColormap();
		}
		else if (te->xproperty.atom == _XA_WM_STATE)
		{
			if (fw && OnThisPage && focus_is_focused(fw) &&
			    FP_DO_FOCUS_ENTER(FW_FOCUS_POLICY(fw)))
			{
				/* refresh the focus - why? */
				focus_force_refresh_focus(fw);
			}
		}
		else
		{
			EWMH_ProcessPropertyNotify(ea->exc);
		}
		break;
	}
}

void HandleReparentNotify(const evh_args_t *ea)
{
	const XEvent *te = ea->exc->x.etrigger;
	FvwmWindow * const fw = ea->exc->w.fw;

	if (!fw)
	{
		return;
	}
	if (te->xreparent.parent == Scr.Root)
	{
		/* Ignore reparenting to the root window.  In some cases these
		 * events are selected although the window is no longer
		 * managed. */
		return;
	}
	if (te->xreparent.parent != FW_W_FRAME(fw))
	{
		/* window was reparented by someone else, destroy the frame */
		SetMapStateProp(fw, WithdrawnState);
		EWMH_RestoreInitialStates(fw, te->type);
		if (!IS_TEAR_OFF_MENU(fw))
		{
			XRemoveFromSaveSet(dpy, te->xreparent.window);
			XSelectInput(dpy, te->xreparent.window, NoEventMask);
		}
		else
		{
			XSelectInput(dpy, te->xreparent.window, XEVMASK_MENUW);
		}
		discard_events(XEVMASK_FRAMEW);
		destroy_window(fw);
		EWMH_ManageKdeSysTray(te->xreparent.window, te->type);
		EWMH_WindowDestroyed();
	}

	return;
}

void HandleSelectionRequest(const evh_args_t *ea)
{
	icccm2_handle_selection_request(ea->exc->x.etrigger);

	return;
}

void HandleSelectionClear(const evh_args_t *ea)
{
	icccm2_handle_selection_clear();

	return;
}

void HandleShapeNotify(const evh_args_t *ea)
{
	FvwmWindow * const fw = ea->exc->w.fw;

	DBUG("HandleShapeNotify","Routine Entered");

	if (FShapesSupported)
	{
		const FShapeEvent *sev =
			(const FShapeEvent *)(ea->exc->x.etrigger);

		if (!fw)
		{
			return;
		}
		if (sev->kind != FShapeBounding)
		{
			return;
		}
		fw->wShaped = sev->shaped;
		frame_setup_shape(fw, fw->frame_g.width, fw->frame_g.height);
	}

	return;
}

void HandleUnmapNotify(const evh_args_t *ea)
{
	int dstx, dsty;
	Window dumwin;
	XEvent dummy;
	const XEvent *te = ea->exc->x.etrigger;
	int weMustUnmap;
	Bool focus_grabbed;
	Bool must_return = False;
	FvwmWindow * const fw = ea->exc->w.fw;

	DBUG("HandleUnmapNotify","Routine Entered");

	/* Don't ignore events as described below. */
	if (te->xunmap.event != te->xunmap.window &&
	   (te->xunmap.event != Scr.Root || !te->xunmap.send_event))
	{
		must_return = True;
	}

	/*
	 * The July 27, 1988 ICCCM spec states that a client wishing to switch
	 * to WithdrawnState should send a synthetic UnmapNotify with the
	 * event field set to (pseudo-)root, in case the window is already
	 * unmapped (which is the case for fvwm for IconicState).
	 * Unfortunately, we looked for the FvwmContext using that field, so
	 * try the window field also. */
	weMustUnmap = 0;
	if (!fw)
	{
		weMustUnmap = 1;
		if (XFindContext(
			    dpy, te->xunmap.window, FvwmContext,
			    (caddr_t *)&fw) == XCNOENT)
		{
			return;
		}
	}
	if (te->xunmap.window == FW_W_FRAME(fw))
	{
		SET_ICONIFY_PENDING(fw , 0);
	}
	if (must_return)
	{
		return;
	}

	if (weMustUnmap)
	{
		unsigned long win = (unsigned long)te->xunmap.window;
		Bool is_map_request_pending;
		check_if_event_args args;

		args.w = win;
		args.do_return_true = False;
		args.do_return_true_cr = False;
		/* Using FCheckTypedWindowEvent() does not work here.  I don't
		 * have the slightest idea why, but using FCheckIfEvent() with
		 * the appropriate predicate procedure works fine. */
		FCheckIfEvent(dpy, &dummy, test_map_request, (char *)&win);
		/* Unfortunately, there is no procedure in X that simply tests
		 * if an event of a certain type in on the queue without
		 * waiting and without removing it from the queue.
		 * XCheck...Event() does not wait but removes the event while
		 * XPeek...() does not remove the event but waits. To solve
		 * this, the predicate procedure sets a flag in the passed in
		 * structure and returns False unconditionally. */
		is_map_request_pending = (args.ret_does_match == True);
		if (!is_map_request_pending)
		{
			XUnmapWindow(dpy, te->xunmap.window);
		}
	}
	if (fw ==  Scr.Hilite)
	{
		Scr.Hilite = NULL;
	}
	focus_grabbed = focus_query_close_release_focus(fw);
	restore_focus_after_unmap(fw, False);
	if (!IS_MAPPED(fw) && !IS_ICONIFIED(fw))
	{
		return;
	}

	/*
	 * The program may have unmapped the client window, from either
	 * NormalState or IconicState.  Handle the transition to WithdrawnState.
	 *
	 * We need to reparent the window back to the root (so that fvwm exiting
	 * won't cause it to get mapped) and then throw away all state (pretend
	 * that we've received a DestroyNotify).
	 */
	if (!FCheckTypedWindowEvent(
		    dpy, te->xunmap.window, DestroyNotify, &dummy) &&
	    XTranslateCoordinates(
		    dpy, te->xunmap.window, Scr.Root, 0, 0, &dstx, &dsty,
		    &dumwin))
	{
		MyXGrabServer(dpy);
		SetMapStateProp(fw, WithdrawnState);
		EWMH_RestoreInitialStates(fw, te->type);
		if (FCheckTypedWindowEvent(
			    dpy, te->xunmap.window, ReparentNotify, &dummy))
		{
			if (fw->attr_backup.border_width)
			{
				XSetWindowBorderWidth(
					dpy, te->xunmap.window,
					fw->attr_backup.border_width);
			}
			if ((!IS_ICON_SUPPRESSED(fw))&&
			   (fw->wmhints &&
			    (fw->wmhints->flags & IconWindowHint)))
			{
				XUnmapWindow(dpy, fw->wmhints->icon_window);
			}
		}
		else
		{
			RestoreWithdrawnLocation(fw, False, Scr.Root);
		}
		if (!IS_TEAR_OFF_MENU(fw))
		{
			XRemoveFromSaveSet(dpy, te->xunmap.window);
			XSelectInput(dpy, te->xunmap.window, NoEventMask);
		}
		XSync(dpy, 0);
		MyXUngrabServer(dpy);
	}
	destroy_window(fw);
	if (focus_grabbed == True)
	{
		CoerceEnterNotifyOnCurrentWindow();
	}
	EWMH_ManageKdeSysTray(te->xunmap.window, te->type);
	EWMH_WindowDestroyed();
	GNOME_SetClientList();

	return;
}

void HandleVisibilityNotify(const evh_args_t *ea)
{
	FvwmWindow * const fw = ea->exc->w.fw;

	DBUG("HandleVisibilityNotify","Routine Entered");

	if (fw && ea->exc->x.etrigger->xvisibility.window == FW_W_FRAME(fw))
	{
		switch (ea->exc->x.etrigger->xvisibility.state)
		{
		case VisibilityUnobscured:
			SET_FULLY_VISIBLE(fw, 1);
			SET_PARTIALLY_VISIBLE(fw, 1);
			break;
		case VisibilityPartiallyObscured:
			SET_FULLY_VISIBLE(fw, 0);
			SET_PARTIALLY_VISIBLE(fw, 1);
			break;
		default:
			SET_FULLY_VISIBLE(fw, 0);
			SET_PARTIALLY_VISIBLE(fw, 0);
			break;
		}
		/* Make sure the button grabs are up to date */
		focus_grab_buttons(fw);
	}

	return;
}

/* ---------------------------- interface functions ------------------------- */

/*
** Procedure:
**   InitEventHandlerJumpTable
*/
void InitEventHandlerJumpTable(void)
{
	int i;

	for (i=0; i<LASTEvent; i++)
	{
		EventHandlerJumpTable[i] = NULL;
	}
	EventHandlerJumpTable[Expose] =           HandleExpose;
	EventHandlerJumpTable[DestroyNotify] =    HandleDestroyNotify;
	EventHandlerJumpTable[MapRequest] =       HandleMapRequest;
	EventHandlerJumpTable[MapNotify] =        HandleMapNotify;
	EventHandlerJumpTable[UnmapNotify] =      HandleUnmapNotify;
	EventHandlerJumpTable[ButtonPress] =      HandleButtonPress;
	EventHandlerJumpTable[EnterNotify] =      HandleEnterNotify;
	EventHandlerJumpTable[LeaveNotify] =      HandleLeaveNotify;
	EventHandlerJumpTable[FocusIn] =          HandleFocusIn;
	EventHandlerJumpTable[FocusOut] =         HandleFocusOut;
	EventHandlerJumpTable[ConfigureRequest] = HandleConfigureRequest;
	EventHandlerJumpTable[ClientMessage] =    HandleClientMessage;
	EventHandlerJumpTable[PropertyNotify] =   HandlePropertyNotify;
	EventHandlerJumpTable[KeyPress] =         HandleKeyPress;
	EventHandlerJumpTable[VisibilityNotify] = HandleVisibilityNotify;
	EventHandlerJumpTable[ColormapNotify] =   HandleColormapNotify;
	if (FShapesSupported)
	{
		EventHandlerJumpTable[FShapeEventBase+FShapeNotify] =
			HandleShapeNotify;
	}
	EventHandlerJumpTable[SelectionClear]   = HandleSelectionClear;
	EventHandlerJumpTable[SelectionRequest] = HandleSelectionRequest;
	EventHandlerJumpTable[ReparentNotify] =   HandleReparentNotify;
	EventHandlerJumpTable[MappingNotify] =    HandleMappingNotify;
	STROKE_CODE(EventHandlerJumpTable[ButtonRelease] = HandleButtonRelease);
	STROKE_CODE(EventHandlerJumpTable[MotionNotify] = HandleMotionNotify);
#ifdef MOUSE_DROPPINGS
	STROKE_CODE(stroke_init(dpy,DefaultRootWindow(dpy)));
#else /* no MOUSE_DROPPINGS */
	STROKE_CODE(stroke_init());
#endif /* MOUSE_DROPPINGS */

	return;
}

/* handle a single X event */
void dispatch_event(XEvent *e)
{
	Window w = e->xany.window;
	FvwmWindow *fw;

	DBUG("dispatch_event","Routine Entered");

	XFlush(dpy);
	if (w == Scr.Root)
	{
		switch (e->type)
		{
		case ButtonPress:
		case ButtonRelease:
			if (e->xbutton.subwindow != None)
			{
				w = e->xbutton.subwindow;
			}
		case MapRequest:
			w = e->xmaprequest.window;
			break;
		default:
			break;
		}
	}
	if (w == Scr.Root ||
	    XFindContext(dpy, w, FvwmContext, (caddr_t *)&fw) == XCNOENT)
	{
		fw = NULL;
	}
	last_event_type = e->type;
	if (EventHandlerJumpTable[e->type])
	{
		evh_args_t ea;
		exec_context_changes_t ecc;
		Window dummyw;

		ecc.type = EXCT_EVENT;
		ecc.x.etrigger = e;
		ecc.w.wcontext = GetContext(&fw, fw, e, &dummyw);
		ecc.w.w = w;
		ecc.w.fw = fw;
		ea.exc = exc_create_context(
			&ecc, ECC_TYPE | ECC_ETRIGGER | ECC_FW | ECC_W |
			ECC_WCONTEXT);
		(*EventHandlerJumpTable[e->type])(&ea);
		exc_destroy_context(ea.exc);
	}

#ifdef C_ALLOCA
	/* If we're using the C version of alloca, see if anything needs to be
	 * freed up.
	 */
	alloca(0);
#endif
	DBUG("dispatch_event","Leaving Routine");

	return;
}

void HandleEvents(void)
{
	XEvent ev;

	DBUG("HandleEvents","Routine Entered");
	STROKE_CODE(send_motion = False);
	while (!isTerminated)
	{
		last_event_type = 0;
		if (Scr.flags.is_window_scheduled_for_destroy)
		{
			destroy_scheduled_windows();
		}
		if (Scr.flags.do_need_window_update)
		{
			flush_window_updates();
		}
		if (Scr.flags.do_need_style_list_update)
		{
			simplify_style_list();
		}
		if (My_XNextEvent(dpy, &ev))
		{
			dispatch_event(&ev);
		}
	}

	return;
}

/***************************************************************************
 *
 * Waits for next X or module event, fires off startup routines when startup
 * modules have finished or after a timeout if the user has specified a
 * command line module that doesn't quit or gets stuck.
 *
 ****************************************************************************/
int My_XNextEvent(Display *dpy, XEvent *event)
{
	extern fd_set_size_t fd_width;
	extern int x_fd;
	fd_set in_fdset, out_fdset;
	Window targetWindow;
	int num_fd;
	int i;
	static struct timeval timeout;
	static struct timeval *timeoutP = &timeout;

	DBUG("My_XNextEvent","Routine Entered");

	/* include this next bit if HandleModuleInput() gets called anywhere
	 * else with queueing turned on.  Because this routine is the only
	 * place that queuing is on _and_ ExecuteCommandQueue is always called
	 * immediately after it is impossible for there to be anything in the
	 * queue at this point */
#if 0
	/* execute any commands queued up */
	DBUG("My_XNextEvent", "executing module comand queue");
	ExecuteCommandQueue();
#endif

	/* check for any X events already queued up.
	 * Side effect: this does an XFlush if no events are queued
	 * Make sure nothing between here and the select causes further
	 * X requests to be sent or the select may block even though
	 * there are events in the queue */
	if (FPending(dpy))
	{
		DBUG("My_XNextEvent","taking care of queued up events"
		     " & returning (1)");
		FNextEvent(dpy, event);
		return 1;
	}

	/* The SIGCHLD signal is sent every time one of our child processes
	 * dies, and the SIGCHLD handler now reaps them automatically. We
	 * should therefore never see a zombie */
#if 0
	DBUG("My_XNextEvent","no X events waiting - about to reap children");
	/* Zap all those zombies! */
	/* If we get to here, then there are no X events waiting to be
	 * processed. Just take a moment to check for dead children. */
	ReapChildren();
#endif

	/* check for termination of all startup modules */
	if (fFvwmInStartup)
	{
		for (i=0;i<npipes;i++)
		{
			if (FD_ISSET(i, &init_fdset))
			{
				break;
			}
		}
		if (i == npipes || writePipes[i+1] == 0)
		{
			DBUG("My_XNextEvent", "Starting up after command"
			     " lines modules\n");
			timeoutP = NULL; /* set an infinite timeout to stop
					  * ticking */
			StartupStuff(); /* This may cause X requests to be sent
					 * */
			return 0; /* so return without select()ing */
		}
	}

	/* Some signals can interrupt us while we wait for any action
	 * on our descriptors. While some of these signals may be asking
	 * fvwm to die, some might be harmless. Harmless interruptions
	 * mean we have to start waiting all over again ... */
	do
	{
		int ms;
		Bool is_waiting_for_scheduled_command = False;
		static struct timeval *old_timeoutP = NULL;

		/* The timeouts become undefined whenever the select returns,
		 * and so we have to reinitialise them */
		ms = squeue_get_next_ms();
		if (ms == 0)
		{
			/* run scheduled commands */
			squeue_execute();
			ms = squeue_get_next_ms();
			/* should not happen anyway.
			 * get_next_schedule_queue_ms() can't return 0 after a
			 * call to execute_schedule_queue(). */
			if (ms == 0)
			{
				ms = 1;
			}
		}
		if (ms < 0)
		{
			timeout.tv_sec = 42;
			timeout.tv_usec = 0;
		}
		else
		{
			/* scheduled commands are pending - don't wait too
			 * long */
			timeout.tv_sec = ms / 1000;
			timeout.tv_usec = 1000 * (ms % 1000);
			old_timeoutP = timeoutP;
			timeoutP = &timeout;
			is_waiting_for_scheduled_command = True;
		}

		FD_ZERO(&in_fdset);
		FD_ZERO(&out_fdset);
		FD_SET(x_fd, &in_fdset);

		/* nothing is done here if fvwm was compiled without session
		 * support */
		if (sm_fd >= 0)
		{
			FD_SET(sm_fd, &in_fdset);
		}

		for (i=0; i<npipes; i++)
		{
			if (readPipes[i]>=0)
			{
				FD_SET(readPipes[i], &in_fdset);
			}
			if (!FQUEUE_IS_EMPTY(&pipeQueue[i]))
			{
				FD_SET(writePipes[i], &out_fdset);
			}
		}

		DBUG("My_XNextEvent","waiting for module input/output");
		num_fd = fvwmSelect(
			fd_width, &in_fdset, &out_fdset, 0, timeoutP);
		if (is_waiting_for_scheduled_command)
		{
			timeoutP = old_timeoutP;
		}

		/* Express route out of FVWM ... */
		if (isTerminated)
		{
			return 0;
		}
	} while (num_fd < 0);

	if (num_fd > 0)
	{
		/* Check for module input. */
		for (i=0; i<npipes; i++)
		{
			if ((readPipes[i] >= 0) &&
			    FD_ISSET(readPipes[i], &in_fdset))
			{
				if (read(readPipes[i], &targetWindow,
					 sizeof(Window)) > 0)
				{
					DBUG("My_XNextEvent",
					     "calling HandleModuleInput");
					/* Add one module message to the queue
					 */
					HandleModuleInput(
						targetWindow, i, NULL, True);
				}
				else
				{
					DBUG("My_XNextEvent",
					     "calling KillModule");
					KillModule(i);
				}
			}
			if ((writePipes[i] >= 0) &&
			    FD_ISSET(writePipes[i], &out_fdset))
			{
				DBUG("My_XNextEvent",
				     "calling FlushMessageQueue");
				FlushMessageQueue(i);
			}
		}

		/* execute any commands queued up */
		DBUG("My_XNextEvent", "executing module comand queue");
		ExecuteCommandQueue();

		/* nothing is done here if fvwm was compiled without session
		 * support */
		if ((sm_fd >= 0) && (FD_ISSET(sm_fd, &in_fdset)))
		{
			ProcessICEMsgs();
		}

	}
	else
	{
		/* select has timed out, things must have calmed down so let's
		 * decorate */
		if (fFvwmInStartup)
		{
			fvwm_msg(ERR, "My_XNextEvent",
				 "Some command line modules have not quit, "
				 "Starting up after timeout.\n");
			StartupStuff();
			timeoutP = NULL; /* set an infinite timeout to stop
					  * ticking */
			reset_style_changes();
			Scr.flags.do_need_window_update = 0;
		}
		/* run scheduled commands if necessary */
		squeue_execute();
	}

	/* check for X events again, rather than return 0 and get called again
	 */
	if (FPending(dpy))
	{
		DBUG("My_XNextEvent",
		     "taking care of queued up events & returning (2)");
		FNextEvent(dpy,event);
		return 1;
	}

	DBUG("My_XNextEvent","leaving My_XNextEvent");
	return 0;
}

/***********************************************************************
 *
 *  Procedure:
 *      Find the Fvwm context for the event.
 *
 ************************************************************************/
int GetContext(FvwmWindow **ret_fw, FvwmWindow *t, const XEvent *e, Window *w)
{
	int context;
	Window win;
	Window subw = None;
	int x = 0;
	int y = 0;
	Bool is_key_event = False;

	win = e->xany.window;
	context = C_NO_CONTEXT;
	switch (e->type)
	{
	case KeyPress:
	case KeyRelease:
		x = e->xkey.x;
		y = e->xkey.y;
		subw = e->xkey.subwindow;
		if (win == Scr.Root && subw != None)
		{
			/* Translate root coordinates into subwindow
			 * coordinates.  Necessary for key bindings that work
			 * over unfocused windows. */
			win = subw;
			XTranslateCoordinates(
				dpy, Scr.Root, subw, x, y, &x, &y, &subw);
			XFindContext(dpy, win, FvwmContext, (caddr_t *) &t);
		}
		is_key_event = True;
		/* fall through */
	case ButtonPress:
	case ButtonRelease:
		if (!is_key_event)
		{
			x = e->xbutton.x;
			y = e->xbutton.y;
			subw = e->xbutton.subwindow;
		}
		if (t && win == FW_W_FRAME(t) && subw != None)
		{
			/* Translate frame coordinates into subwindow
			 * coordinates. */
			win = subw;
			XTranslateCoordinates(
				dpy, FW_W_FRAME(t), subw, x, y, &x, &y, &subw);
			if (win == FW_W_PARENT(t))
			{
				win = subw;
				XTranslateCoordinates(
					dpy, FW_W_PARENT(t), subw, x, y, &x,
					&y, &subw);
			}
		}
		break;
	default:
		XFindContext(dpy, win, FvwmContext, (caddr_t *)&t);
		break;
	}
	if (ret_fw != NULL)
	{
		*ret_fw = t;
	}
	if (!t)
	{
		return C_ROOT;
	}
	*w = win;
	if (*w == Scr.NoFocusWin)
	{
		return C_ROOT;
	}
	if (subw != None)
	{
		if (win == FW_W_PARENT(t))
		{
			*w = subw;
		}
	}
	if (*w == Scr.Root)
	{
		return C_ROOT;
	}
	context = frame_window_id_to_context(t, *w, &Button);

	return context;
}

/**************************************************************************
 *
 * Removes expose events for a specific window from the queue
 *
 *************************************************************************/
int flush_expose(Window w)
{
	XEvent dummy;
	int i=0;

	while (FCheckTypedWindowEvent(dpy, w, Expose, &dummy))
	{
		i++;
	}

	return i;
}

/* same as above, but merges the expose rectangles into a single big one */
int flush_accumulate_expose(Window w, XEvent *e)
{
	XEvent dummy;
	int i = 0;
	int x1 = e->xexpose.x;
	int y1 = e->xexpose.y;
	int x2 = x1 + e->xexpose.width;
	int y2 = y1 + e->xexpose.height;

	while (FCheckTypedWindowEvent(dpy, w, Expose, &dummy))
	{
		x1 = min(x1, dummy.xexpose.x);
		y1 = min(y1, dummy.xexpose.y);
		x2 = max(x2, dummy.xexpose.x + dummy.xexpose.width);
		y2 = max(y2, dummy.xexpose.y + dummy.xexpose.height);
		i++;
	}
	e->xexpose.x = x1;
	e->xexpose.y = y1;
	e->xexpose.width = x2 - x1;
	e->xexpose.height = y2 - y1;

	return i;
}

/**************************************************************************
 *
 * Removes all expose events from the queue and does the necessary redraws
 *
 *************************************************************************/
void handle_all_expose(void)
{
	void *saved_event;
	XEvent evdummy;

	saved_event = fev_save_event();
	FPending(dpy);
	while (FCheckMaskEvent(dpy, ExposureMask, &evdummy))
	{
		dispatch_event(&evdummy);
	}
	fev_restore_event(saved_event);

	return;
}

/* CoerceEnterNotifyOnCurrentWindow()
 * Pretends to get a HandleEnterNotify on the window that the pointer
 * currently is in so that the focus gets set correctly from the beginning.
 * Note that this presently only works if the current window is not
 * click_to_focus;  I think that that behaviour is correct and desirable.
 * --11/08/97 gjb */
void CoerceEnterNotifyOnCurrentWindow(void)
{
	Window child;
	Window root;
	Bool f;
	evh_args_t ea;
	exec_context_changes_t ecc;
	XEvent e;
	FvwmWindow *fw;

	f = FQueryPointer(
		dpy, Scr.Root, &root, &child, &e.xcrossing.x_root,
		&e.xcrossing.y_root, &e.xcrossing.x, &e.xcrossing.y,
		&JunkMask);
	if (f == False || child == None)
	{
		return;
	}
	e.xcrossing.type = EnterNotify;
	e.xcrossing.window = child;
	e.xcrossing.subwindow = None;
	e.xcrossing.mode = NotifyNormal;
	e.xcrossing.detail = NotifyAncestor;
	e.xcrossing.same_screen = True;
	if (XFindContext(dpy, child, FvwmContext, (caddr_t *)&fw) == XCNOENT)
	{
		fw = NULL;
	}
	else
	{
		XTranslateCoordinates(
			dpy, Scr.Root, child, e.xcrossing.x_root,
			e.xcrossing.y_root, &JunkX, &JunkY, &child);
		if (child == FW_W_PARENT(fw))
		{
			child = FW_W(fw);
		}
		if (child != None)
		{
			e.xany.window = child;
		}
	}
	e.xcrossing.focus = (fw == get_focus_window()) ? True : False;
	ecc.type = EXCT_NULL;
	ecc.x.etrigger = &e;
	ea.exc = exc_create_context(&ecc, ECC_TYPE | ECC_ETRIGGER);
	HandleEnterNotify(&ea);
	exc_destroy_context(ea.exc);

	return;
}

/* This function discards all queued up ButtonPress, ButtonRelease and
 * ButtonMotion events. */
int discard_events(long event_mask)
{
	XEvent e;
	int count;

	XSync(dpy, 0);
	for (count = 0; FCheckMaskEvent(dpy, event_mask, &e); count++)
	{
		/* nothing */
	}

	return count;
}

/* This function discards all queued up ButtonPress, ButtonRelease and
 * ButtonMotion events. */
int discard_window_events(Window w, long event_mask)
{
	XEvent e;
	int count;

	XSync(dpy, 0);
	for (count = 0; FCheckWindowEvent(dpy, w, event_mask, &e); count++)
	{
		/* nothing */
	}

	return count;
}

/* Similar function for certain types of PropertyNotify. */
int flush_property_notify(Atom atom, Window w)
{
	XEvent e;
	int count;

	XSync(dpy, 0);
	for (count = 0; FCheckTypedWindowEvent(dpy, w, PropertyNotify, &e);
	     count++)
	{
		if (e.xproperty.atom != atom)
		{
			FPutBackEvent(dpy, &e);
			break;
		}
	}

	return count;
}

/***************************************************************************
 *
 * Wait for all mouse buttons to be released
 * This can ease some confusion on the part of the user sometimes
 *
 * Discard superflous button events during this wait period.
 *
 ***************************************************************************/
void WaitForButtonsUp(Bool do_handle_expose)
{
	unsigned int mask;
	unsigned int bmask;
	long evmask = ButtonPressMask|ButtonReleaseMask|ButtonMotionMask|
		KeyPressMask|KeyReleaseMask;
	unsigned int count;
	int use_wait_cursor;
	XEvent e;

	if (FQueryPointer(dpy, Scr.Root, &JunkRoot, &JunkChild, &JunkX, &JunkY,
			  &JunkX, &JunkY, &mask) == False)
	{
		/* pointer is on a different screen - that's okay here */
	}
	if ((mask & (DEFAULT_ALL_BUTTONS_MASK)) == 0)
	{
		return;
	}
	if (do_handle_expose)
	{
		evmask |= ExposureMask;
	}
	GrabEm(None, GRAB_NORMAL);
	for (count = 0, use_wait_cursor = 0; mask & (DEFAULT_ALL_BUTTONS_MASK);
	     count++)
	{
		/* handle expose events */
		XAllowEvents(dpy, SyncPointer, CurrentTime);
		if (FCheckMaskEvent(dpy, evmask, &e))
		{
			switch (e.type)
			{
			case ButtonRelease:
				bmask = (Button1Mask << (e.xbutton.button - 1));
				mask = e.xbutton.state & ~bmask;
				break;
			case Expose:
				dispatch_event(&e);
				break;
			default:
				break;
			}
		}
		else
		{
			if (FQueryPointer(
				    dpy, Scr.Root, &JunkRoot, &JunkChild,
				    &JunkX, &JunkY, &JunkX, &JunkY, &mask) ==
			    False)
			{
				/* pointer is on a different screen - that's
				 * okay here */
			}
			usleep(1);
		}
		if (use_wait_cursor == 0 && count == 20)
		{
			GrabEm(CRS_WAIT, GRAB_NORMAL);
			use_wait_cursor = 1;
		}
	}
	UngrabEm(GRAB_NORMAL);
	if (use_wait_cursor)
	{
		UngrabEm(GRAB_NORMAL);
		XFlush(dpy);
	}

	return;
}

void sync_server(int toggle)
{
	static Bool synced = False;

	if (toggle == -1)
	{
		toggle = (synced == False);
	}
	if (toggle == 1)
	{
		synced = True;
	}
	else
	{
		synced = False;
	}
	XSynchronize(dpy, synced);
	XFlush(dpy);

	return;
}

Bool is_resizing_event_pending(
	FvwmWindow *fw)
{
	XEvent e;
	check_if_event_args args;

	args.w = FW_W(fw);
	args.do_return_true = False;
	args.do_return_true_cr = False;
	args.cr_value_mask = 0;
	args.ret_does_match = False;
	args.ret_type = 0;
	FCheckIfEvent(dpy, &e, test_resizing_event, (char *)&args);

	return args.ret_does_match;
}

/* ---------------------------- builtin commands ---------------------------- */

void CMD_XSynchronize(F_CMD_ARGS)
{
	int toggle;

	toggle = ParseToggleArgument(action, NULL, -1, 0);
	sync_server(toggle);

	return;
}

#if 0
/* experimental code */
int addkbsubinstoarray(Window parentw, Window *winlist, int offset)
{
	Window *children;
	int i;
	int n;
	int s;
	Window JunkW;
	static int depth = 0;

	depth++;
	if (XQueryTree(dpy, parentw, &JunkW, &JunkW, &children, &n) == 0)
	{
		depth--;
		return offset;
	}
/*fprintf(stderr,"%d: 0x%08x has %d children\n", depth, (int)parentw, n);*/
	if (n == 0)
	{
		depth--;
		return offset;
	}
	for (i = 0; i < n; i++)
	{
		XWindowAttributes xwa;

		if (XGetWindowAttributes(dpy, children[i], &xwa) == 0)
		{
			continue;
		}
#if 0
		if (xwa.map_state != IsViewable)
		{
			continue;
		}
#endif
#define KBWIN_MIN_WIDTH 100
#define KBWIN_MIN_HEIGHT 23
#define MASK (KeyPressMask|KeyReleaseMask)
		s = addkbsubinstoarray(children[i], winlist, offset);
		if ((xwa.all_event_masks & MASK) &&
		    xwa.width >= KBWIN_MIN_WIDTH &&
		    xwa.height >= KBWIN_MIN_HEIGHT &&
		    s == offset)
		{
fprintf(stderr, "w %d (%d), h %d (%d)\n", xwa.width, KBWIN_MIN_WIDTH, xwa.height, KBWIN_MIN_HEIGHT);
			winlist[offset] = children[i];
			offset++;
fprintf(stderr,"%d: + 0x%08x offset %d\n", depth, (int)children[i], offset);
		}
#if 1
		else if (depth == 1 && i + 1 == n)
		{
			offset = s;
			winlist[offset] = parentw;
			offset++;
fprintf(stderr,"%d: * 0x%08x offset %d\n", depth, (int)parentw, offset);
		}
#endif
		else
		{
/*fprintf(stderr,"%d: v 0x%08x s %d offset %d ev 0x%x 0x%x 0x%x\n", depth, (int)children[i], s, offset, (int)xwa.all_event_masks, (int)MASK, (int)(xwa.all_event_masks & MASK));*/
			offset = s;
		}
	}
	XFree(children);
	depth--;

	return offset;
}

int findcurrentkbwininlist(Window start_win, Window *winlist, int n)
{
	int x;
	int y;
	int new_x;
	int new_y;
	int i;
	int found = -1;
	Window child;
	Window new_child;

/*fprintf(stderr,"find sw = 0x%08x\n", (int)start_win);*/
	if (FQueryPointer(
		    dpy, start_win, &JunkRoot, &child, &JunkX, &JunkY, &x, &y,
		    &JunkMask) == False)
	{
		return None;
	}
	while (start_win != None)
	{
/*fprintf(stderr,"checking 0x%08x %d\n", (int)start_win, n);*/
		for (i = 0; i < n; i++)
		{
			if (winlist[i] == start_win)
			{
fprintf(stderr,"is in 0x%08x\n", (int)start_win);
				found = i;
				break;
			}
		}
		if (child != None && XTranslateCoordinates(
			    dpy, start_win, child, x, y, &new_x, &new_y,
			    &new_child) == False)
		{
			break;
		}
		start_win = child;
		child = new_child;
		x = new_x;
		y = new_y;
	}

	return found;
}

void getkbsubwins(F_CMD_ARGS)
{
	Window win;
	Window focus_win;
	Window winlist[1000];
	static Window prev_win = None;
	static Window last_win = None;
	int n;
	int i;
	int j;
	char cmd[256];
	int x;
	int y;
	int wid;
	int hei;

	if (!fw)
	{
		return;
	}
	win = FW_W(fw);
fprintf(stderr,"start 0x%08x prev 0x%08x last 0x%08x\n", (int)win, (int)prev_win, (int)last_win);
	n = addkbsubinstoarray(win, &(winlist[0]), 0);
	if (n == 0)
	{
		return;
	}
	XGetInputFocus(dpy, &focus_win, &i);
/*fprintf(stderr,"focus = 0x%08x\n",(int)focus_win);*/
	if (focus_win == None)
	{
		return;
	}
	win = focus_win;
	i = findcurrentkbwininlist(win, &(winlist[0]), n);
	if (i < 0)
	{
		XBell(dpy, 0);
		return;
	}
	if (winlist[i] != last_win && last_win != None && XGetGeometry(
		    dpy, last_win, &JunkRoot, &JunkX, &JunkY, &JunkWidth,
		    &JunkHeight, &JunkBW, &JunkMask) != 0)
	{
		for (j = 0; j < n; j++)
		{
			if (winlist[j] == last_win)
			{
				i = j;
				break;
			}
		}
	}
	prev_win = winlist[i];
XGetGeometry(dpy, winlist[i], &JunkRoot, &x,&y,&wid,&hei,&JunkBW, &JunkMask);
fprintf(stderr,"  %d (%d %d %dx%d) -> ", i, x,y,wid,hei);
	i++;
	if (i >= n)
	{
		i = 0;
	}
	if (winlist[i] == focus_win && n > 1)
	{
		i++;
		if (i >= n)
		{
			i = 0;
		}
	}
	win = winlist[i];
XGetGeometry(dpy, win, &JunkRoot, &x,&y,&wid,&hei,&JunkBW, &JunkMask);
fprintf(stderr," %d (%d %d %dx%d)\n", i, x,y,wid,hei);
	sprintf(cmd, "WindowId 0x%x WarpToWindow 99 99", (int)win);
	fprintf(stderr, "%s\n", cmd);
	execute_function(NULL, exc, cmd, 0);
	last_win = win;
	/*!!!*/

	return;
}
#endif

void CMD_XSync(F_CMD_ARGS)
{
	XSync(dpy, 0);

	return;
}
