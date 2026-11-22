/* Faster standard VX Ace events: Game_Event#refresh (with find_proper_page
 * and conditions_met?) in C. On Dazed's street map (108 events) Ruby spent
 * a third of its time there: every switch or variable change asks the map
 * to refresh, and some events change one every frame.
 *
 * Like Sprite_Character#update (scene.c): only while the methods it stands
 * for are the standard ones (boot.rb compares their source text, and asks
 * again for every class it meets: Game_Event subclasses, the classes of
 * $game_switches, $game_variables, $game_self_switches). Conditions it
 * doesn't handle (item, actor, a variable that isn't an Integer) and
 * setup_page call the original Ruby methods. */
#include "engine.h"

static ID id_fast_ok, id_slow_refresh, id_setup_page, id_conditions_met, id_lt,
	gv_switches, gv_variables, gv_self_switches,
	iv_erased, iv_page, iv_event, iv_pages, iv_id, iv_map_id, iv_data, iv_condition,
	iv_switch1_valid, iv_switch1_id, iv_switch2_valid, iv_switch2_id, iv_variable_valid,
	iv_variable_id, iv_variable_value, iv_self_switch_valid, iv_self_switch_ch, iv_item_valid,
	iv_actor_valid;

/* kinds of classes boot.rb checks (MKXPZ_FAST_NEEDS) */
enum { K_EVENT, K_SWITCHES, K_VARIABLES, K_SELF_SWITCHES, K_COUNT };
static const char *const kind_names[K_COUNT] = { "event", "switches", "variables", "self_switches" };

static struct { VALUE klass; int kind; bool ok; } cache[32];
static int ncache;

static bool fast_ok(VALUE obj, int kind)
{
	if (RB_SPECIAL_CONST_P(obj))
		return false;
	VALUE klass = CLASS_OF(obj);
	if (FL_TEST(klass, FL_SINGLETON))
		return false;
	for (int i = 0; i < ncache; i++)
		if (cache[i].klass == klass && cache[i].kind == kind)
			return cache[i].ok;
	bool ok = RTEST(rb_funcall(Qnil, id_fast_ok, 2, klass, ID2SYM(rb_intern(kind_names[kind]))));
	if (ncache < (int) (sizeof cache / sizeof *cache)) {
		rb_gc_register_mark_object(klass);
		cache[ncache].klass = klass;
		cache[ncache].kind = kind;
		cache[ncache++].ok = ok;
	}
	return ok;
}

/* the globals, checked once per refresh of the whole map is enough, but
 * they're cheap: by ID, and their class from the cache */
typedef struct { VALUE sw, var, self_sw; } Globals;

static bool globals(Globals *g)
{
	VALUE s = mkxpz_gvar(gv_switches), v = mkxpz_gvar(gv_variables), ss = mkxpz_gvar(gv_self_switches);
	if (!fast_ok(s, K_SWITCHES) || !fast_ok(v, K_VARIABLES) || !fast_ok(ss, K_SELF_SWITCHES))
		return false;
	g->sw = IV(s, iv_data);
	g->var = IV(v, iv_data);
	g->self_sw = IV(ss, iv_data);
	return RB_TYPE_P(g->sw, T_ARRAY) && RB_TYPE_P(g->var, T_ARRAY) && RB_TYPE_P(g->self_sw, T_HASH);
}

/* $game_switches[id]: @data[id] || false */
static bool switch_on(const Globals *g, VALUE id)
{
	return FIXNUM_P(id) && RTEST(rb_ary_entry(g->sw, FIX2LONG(id)));
}

/* 1 met, 0 not met, -1 ask Ruby */
static int conditions_met(VALUE self, VALUE event, VALUE page, const Globals *g)
{
	VALUE c = IV(page, iv_condition);
	if (!RB_TYPE_P(c, T_OBJECT))
		return -1;
	if (RTEST(IV(c, iv_switch1_valid)) && !switch_on(g, IV(c, iv_switch1_id)))
		return 0;
	if (RTEST(IV(c, iv_switch2_valid)) && !switch_on(g, IV(c, iv_switch2_id)))
		return 0;
	if (RTEST(IV(c, iv_variable_valid))) {
		/* $game_variables[id] < value; @data[id] || 0 */
		VALUE id = IV(c, iv_variable_id), want = IV(c, iv_variable_value);
		if (!FIXNUM_P(id))
			return -1;
		VALUE v = rb_ary_entry(g->var, FIX2LONG(id));
		if (!RTEST(v))
			v = INT2FIX(0);
		if (!FIXNUM_P(v) || !FIXNUM_P(want))
			return -1; /* (Ruby's < for anything else, errors included) */
		if (FIX2LONG(v) < FIX2LONG(want))
			return 0;
	}
	if (RTEST(IV(c, iv_self_switch_valid))) {
		/* $game_self_switches[[@map_id, @event.id, ch]] == true */
		VALUE key = rb_ary_new_from_args(3, IV(self, iv_map_id), IV(event, iv_id), IV(c, iv_self_switch_ch));
		if (rb_hash_lookup2(g->self_sw, key, Qnil) != Qtrue)
			return 0;
	}
	if (RTEST(IV(c, iv_item_valid)) || RTEST(IV(c, iv_actor_valid)))
		return -1;
	return 1;
}

/* find_proper_page: the last page whose conditions are met (nil: none);
 * Qundef: not here (the caller runs the Ruby method) */
static VALUE find_page(VALUE self, const Globals *g)
{
	VALUE event = IV(self, iv_event);
	if (!RB_TYPE_P(event, T_OBJECT))
		return Qundef;
	VALUE pages = IV(event, iv_pages);
	if (!RB_TYPE_P(pages, T_ARRAY))
		return Qundef;
	for (long i = RARRAY_LEN(pages) - 1; i >= 0; i--) {
		VALUE page = RARRAY_AREF(pages, i);
		int met = RB_TYPE_P(page, T_OBJECT) ? conditions_met(self, event, page, g) : -1;
		if (met < 0)
			met = RTEST(rb_funcall(self, id_conditions_met, 1, page));
		if (met)
			return page;
	}
	return Qnil;
}

static VALUE fast_refresh(VALUE self)
{
	Globals g;
	if (!fast_ok(self, K_EVENT) || !globals(&g))
		return rb_funcall(self, id_slow_refresh, 0);
	/* new_page = @erased ? nil : find_proper_page */
	VALUE new_page = RTEST(IV(self, iv_erased)) ? Qnil : find_page(self, &g);
	if (new_page == Qundef)
		return rb_funcall(self, id_slow_refresh, 0);
	/* setup_page(new_page) if !new_page || new_page != @page (RPG pages
	 * compare by identity) */
	if (NIL_P(new_page) || new_page != IV(self, iv_page))
		return rb_funcall(self, id_setup_page, 1, new_page);
	return Qnil;
}

/* (tests: the page the C code finds, to compare with find_proper_page) */
static VALUE fast_find_page(VALUE self)
{
	Globals g;
	if (!globals(&g))
		return Qfalse;
	VALUE p = find_page(self, &g);
	return p == Qundef ? Qfalse : p;
}

/* (boot.rb, once it checked the scripts) */
static VALUE fast_install_events(VALUE self, VALUE klass)
{
	(void) self;
	rb_define_method(klass, "refresh", fast_refresh, 0);
	rb_define_method(klass, "mkxpz_fast_find_page", fast_find_page, 0);
	return Qtrue;
}

/* --- "Anti Lag VX" (Anaryu, used by many VX games, e.g. Changed):
 * Game_Event#on_screen, asked twice per event every frame (Changed: 269
 * events, half of the game's time). boot.rb installs this only while the
 * method's text is the script's (checksum) and Game_Map#display_x/y are
 * plain readers. Values it doesn't handle: the Ruby method. */
static ID id_slow_on_screen, iv_width, iv_height, iv_real_x, iv_real_y, iv_xov, iv_yov, iv_xos, iv_yos,
	iv_display_x, iv_display_y, gv_game_map;
static VALUE antilag_map_class;

static VALUE fast_on_screen(VALUE self)
{
	VALUE w = IV(self, iv_width), h = IV(self, iv_height);
	if (!FIXNUM_P(w) || !FIXNUM_P(h))
		return rb_funcall(self, id_slow_on_screen, 0); /* (sizes not known yet) */
	VALUE xov = IV(self, iv_xov), yov = IV(self, iv_yov);
	if (NIL_P(xov) || NIL_P(yov))
		return Qtrue;
	VALUE gm = mkxpz_gvar(gv_game_map);
	VALUE xos = IV(self, iv_xos), yos = IV(self, iv_yos), rx = IV(self, iv_real_x), ry = IV(self, iv_real_y);
	if (RB_SPECIAL_CONST_P(gm) || CLASS_OF(gm) != antilag_map_class)
		return rb_funcall(self, id_slow_on_screen, 0);
	VALUE dx = IV(gm, iv_display_x), dy = IV(gm, iv_display_y);
	if (!FIXNUM_P(xov) || !FIXNUM_P(yov) || !FIXNUM_P(xos) || !FIXNUM_P(yos) || !FIXNUM_P(rx) || !FIXNUM_P(ry) ||
	    !FIXNUM_P(dx) || !FIXNUM_P(dy))
		return rb_funcall(self, id_slow_on_screen, 0);
	/* (Ruby's integer division rounds down) */
	long hw = FIX2LONG(w) >= 0 ? FIX2LONG(w) / 2 : -((-FIX2LONG(w) + 1) / 2);
	long hh = FIX2LONG(h) >= 0 ? FIX2LONG(h) / 2 : -((-FIX2LONG(h) + 1) / 2);
	long x = FIX2LONG(rx), y = FIX2LONG(ry), d = FIX2LONG(dx), e = FIX2LONG(dy);
	bool xin = x <= d + FIX2LONG(xov) + hw && x >= d - FIX2LONG(xos) - hw;
	bool yin = y <= e + FIX2LONG(yov) + hh && y >= e - FIX2LONG(yos) - hh;
	return xin && yin ? Qtrue : Qfalse;
}

/* "Anti Lag VX"'s per-frame loops: Game_Event#update (with update?) and
 * Spriteset_Map#update_characters, calling the native on_screen, event and
 * sprite updates directly (each ~2 us less than a Ruby call, 3-4 calls per
 * event every frame). Only for classes boot.rb confirms (mkxpz_antilag_ok?):
 * that their methods resolve to exactly these native ones, and that
 * on_old / on_new / character are plain accessors; else a Ruby call. */
VALUE mkxpz_sprite_update_vx(VALUE sprite); /* scene.c */
VALUE mkxpz_event_update_vx(VALUE event);
static ID id_antilag_ok, id_slow_antilag_update, id_slow_update_characters, id_on_screen, id_update_q,
	id_pre_antilag_update, id_update, id_on_old_set, iv_ignore_update, iv_allow_screen_ignore, iv_force_update,
	iv_move_route_forcing, iv_character_sprites, iv_character, iv_on_old, iv_on_new;
enum { A_EVENT, A_ATTRS, A_SPRITE, A_COUNT };
static const char *const antilag_kinds[A_COUNT] = { "event", "attrs", "sprite" };
static struct { VALUE klass; int kind; bool ok; } acache[48];
static int nacache;

static bool antilag_ok(VALUE obj, int kind)
{
	if (RB_SPECIAL_CONST_P(obj))
		return false;
	VALUE klass = CLASS_OF(obj);
	if (FL_TEST(klass, FL_SINGLETON))
		return false;
	for (int i = 0; i < nacache; i++)
		if (acache[i].klass == klass && acache[i].kind == kind)
			return acache[i].ok;
	bool ok = RTEST(rb_funcall(Qnil, id_antilag_ok, 2, klass, ID2SYM(rb_intern(antilag_kinds[kind]))));
	if (nacache < (int) (sizeof acache / sizeof *acache)) {
		rb_gc_register_mark_object(klass);
		acache[nacache].klass = klass;
		acache[nacache].kind = kind;
		acache[nacache++].ok = ok;
	}
	return ok;
}

static VALUE on_screen_of(VALUE chr)
{
	return antilag_ok(chr, A_EVENT) ? fast_on_screen(chr) : rb_funcall(chr, id_on_screen, 0);
}

/* def update; if update?; pre_antilag_update; end; end */
static VALUE fast_antilag_update(VALUE self)
{
	if (!antilag_ok(self, A_EVENT))
		return rb_funcall(self, id_slow_antilag_update, 0);
	/* update?: ignore = ((not @ignore_update) and (on_screen and @allow_screen_ignore))
	 *          @force_update or ignore or @move_route_forcing */
	bool ignore = false;
	if (!RTEST(IV(self, iv_ignore_update))) {
		VALUE on = fast_on_screen(self);
		ignore = RTEST(on) && RTEST(IV(self, iv_allow_screen_ignore));
	}
	if (RTEST(IV(self, iv_force_update)) || ignore || RTEST(IV(self, iv_move_route_forcing)))
		return mkxpz_event_update_vx(self);
	return Qnil;
}

/* for sprite in @character_sprites
 *   on = sprite.character.on_screen
 *   sprite.update if on || sprite.character.on_old = true
 *   sprite.character.on_old = sprite.character.on_new
 * end */
static VALUE fast_update_characters(VALUE self)
{
	VALUE list = IV(self, iv_character_sprites);
	if (!RB_TYPE_P(list, T_ARRAY))
		return rb_funcall(self, id_slow_update_characters, 0);
	for (long i = 0; i < RARRAY_LEN(list); i++) {
		VALUE sprite = RARRAY_AREF(list, i);
		bool direct = antilag_ok(sprite, A_SPRITE);
		VALUE chr = direct ? IV(sprite, iv_character) : rb_funcall(sprite, rb_intern("character"), 0);
		bool attrs = antilag_ok(chr, A_ATTRS);
		VALUE on = on_screen_of(chr);
		if (!RTEST(on)) {
			if (attrs)
				rb_ivar_set(chr, iv_on_old, Qtrue);
			else
				rb_funcall(chr, id_on_old_set, 1, Qtrue);
		}
		if (direct)
			mkxpz_sprite_update_vx(sprite);
		else
			rb_funcall(sprite, id_update, 0);
		if (!direct)
			chr = rb_funcall(sprite, rb_intern("character"), 0);
		else
			chr = IV(sprite, iv_character);
		if (antilag_ok(chr, A_ATTRS))
			rb_ivar_set(chr, iv_on_old, IV(chr, iv_on_new));
		else
			rb_funcall(chr, id_on_old_set, 1, rb_funcall(chr, rb_intern("on_new"), 0));
	}
	return list;
}

/* Game_Map#update_events (VX's standard): each event's update called
 * directly when it is one of the engine's (event_update_kind: which one
 * Game_Event#update is), the common events in Ruby */
VALUE rb_hash_values(VALUE hash); /* (Ruby's hash.c; only in its internal headers) */
static int event_update_kind; /* 0 Ruby, 1 "Anti Lag VX"'s (events.c), 2 VX's (scene.c) */
static ID id_slow_update_events, iv_events, iv_common_events, id_events_direct_ok;
static VALUE event_update_class;

static VALUE update_event(VALUE e)
{
	if (event_update_kind && !RB_SPECIAL_CONST_P(e) && CLASS_OF(e) == event_update_class)
		return event_update_kind == 1 ? fast_antilag_update(e) : mkxpz_event_update_vx(e);
	return rb_funcall(e, id_update, 0);
}

static VALUE fast_update_events(VALUE self)
{
	VALUE ev = IV(self, iv_events), ce = IV(self, iv_common_events);
	if (!RB_TYPE_P(ev, T_HASH) || !RB_TYPE_P(ce, T_HASH))
		return rb_funcall(self, id_slow_update_events, 0);
	VALUE list = rb_hash_values(ev); /* (a snapshot, as @events.values) */
	for (long i = 0; i < RARRAY_LEN(list); i++)
		update_event(RARRAY_AREF(list, i));
	list = rb_hash_values(ce);
	for (long i = 0; i < RARRAY_LEN(list); i++)
		rb_funcall(RARRAY_AREF(list, i), id_update, 0);
	RB_GC_GUARD(list);
	return Qnil;
}

/* Game_Interpreter#setup_starting_event (VX's standard): the event loop
 * (each event's @starting) in C; refresh, a reserved common event and
 * the autorun common events in Ruby (mkxpz_setup_starting_rest) */
static ID id_slow_setup_starting, id_setup_starting_rest, id_start_event, iv_need_refresh, iv_starting,
	gv_game_temp, iv_common_event_id, id_refresh;
static VALUE starting_map_class, starting_temp_class, starting_event_class;

static VALUE fast_setup_starting_event(VALUE self)
{
	VALUE gm = mkxpz_gvar(gv_game_map), gt = mkxpz_gvar(gv_game_temp);
	if (RB_SPECIAL_CONST_P(gm) || CLASS_OF(gm) != starting_map_class || RB_SPECIAL_CONST_P(gt) ||
	    CLASS_OF(gt) != starting_temp_class)
		return rb_funcall(self, id_slow_setup_starting, 0);
	if (RTEST(IV(gm, iv_need_refresh)))
		rb_funcall(gm, id_refresh, 0);
	VALUE ce = IV(gt, iv_common_event_id);
	if (!FIXNUM_P(ce) || FIX2LONG(ce) > 0)
		return rb_funcall(self, id_slow_setup_starting, 0); /* (a reserved common event: all in Ruby) */
	VALUE ev = IV(gm, iv_events);
	if (!RB_TYPE_P(ev, T_HASH))
		return rb_funcall(self, id_slow_setup_starting, 0);
	VALUE list = rb_hash_values(ev);
	for (long i = 0; i < RARRAY_LEN(list); i++) {
		VALUE e = RARRAY_AREF(list, i);
		VALUE st = !RB_SPECIAL_CONST_P(e) && CLASS_OF(e) == starting_event_class ? IV(e, iv_starting)
		                                                                       : rb_funcall(e, rb_intern("starting"), 0);
		if (RTEST(st))
			return rb_funcall(self, id_start_event, 1, e); /* clear_starting; setup(event.list, event.id) */
	}
	RB_GC_GUARD(list);
	return rb_funcall(self, id_setup_starting_rest, 0);
}

static VALUE fast_install_vx_map_loops(VALUE self, VALUE map_class, VALUE interp_class, VALUE temp_class,
                                       VALUE event_class, VALUE kind)
{
	(void) self;
	event_update_kind = NUM2INT(kind);
	event_update_class = event_class;
	rb_gc_register_mark_object(event_class);
	if (RTEST(map_class))
		rb_define_method(map_class, "update_events", fast_update_events, 0);
	if (RTEST(interp_class)) {
		starting_map_class = rb_obj_class(mkxpz_gvar(gv_game_map)) == rb_cNilClass ? map_class
		                                                                         : CLASS_OF(mkxpz_gvar(gv_game_map));
		starting_temp_class = temp_class;
		starting_event_class = event_class;
		rb_gc_register_mark_object(temp_class);
		rb_define_method(interp_class, "setup_starting_event", fast_setup_starting_event, 0);
	}
	return Qtrue;
}

static VALUE fast_install_antilag_loops(VALUE self, VALUE event_class, VALUE spriteset_class)
{
	(void) self;
	if (RTEST(event_class))
		rb_define_method(event_class, "update", fast_antilag_update, 0);
	if (RTEST(spriteset_class))
		rb_define_method(spriteset_class, "update_characters", fast_update_characters, 0);
	return Qtrue;
}

static VALUE fast_install_antilag(VALUE self, VALUE event_class, VALUE map_class)
{
	(void) self;
	antilag_map_class = map_class;
	rb_gc_register_mark_object(map_class);
	rb_define_method(event_class, "on_screen", fast_on_screen, 0);
	return Qtrue;
}

void init_events(void)
{
#define I(v, s) v = rb_intern(s)
	I(id_fast_ok, "mkxpz_fast_ok?"); I(id_slow_refresh, "mkxpz_slow_refresh");
	I(id_setup_page, "setup_page"); I(id_conditions_met, "conditions_met?"); I(id_lt, "<");
	I(gv_switches, "$game_switches"); I(gv_variables, "$game_variables"); I(gv_self_switches, "$game_self_switches");
	I(iv_erased, "@erased"); I(iv_page, "@page"); I(iv_event, "@event"); I(iv_pages, "@pages"); I(iv_id, "@id");
	I(iv_map_id, "@map_id"); I(iv_data, "@data"); I(iv_condition, "@condition");
	I(iv_switch1_valid, "@switch1_valid"); I(iv_switch1_id, "@switch1_id");
	I(iv_switch2_valid, "@switch2_valid"); I(iv_switch2_id, "@switch2_id");
	I(iv_variable_valid, "@variable_valid"); I(iv_variable_id, "@variable_id"); I(iv_variable_value, "@variable_value");
	I(iv_self_switch_valid, "@self_switch_valid"); I(iv_self_switch_ch, "@self_switch_ch");
	I(iv_item_valid, "@item_valid"); I(iv_actor_valid, "@actor_valid");
	rb_define_global_function("mkxpz_fast_install_events", fast_install_events, 1);
	I(id_slow_on_screen, "mkxpz_slow_on_screen"); I(iv_width, "@width"); I(iv_height, "@height");
	I(iv_real_x, "@real_x"); I(iv_real_y, "@real_y"); I(iv_xov, "@x_offscreen_value");
	I(iv_yov, "@y_offscreen_value"); I(iv_xos, "@x_offscreen_squares"); I(iv_yos, "@y_offscreen_squares");
	I(iv_display_x, "@display_x"); I(iv_display_y, "@display_y"); I(gv_game_map, "$game_map");
	rb_define_global_function("mkxpz_fast_install_antilag", fast_install_antilag, 2);
	I(id_antilag_ok, "mkxpz_antilag_ok?"); I(id_slow_antilag_update, "mkxpz_slow_antilag_update");
	I(id_slow_update_characters, "mkxpz_slow_update_characters"); I(id_on_screen, "on_screen");
	I(id_update_q, "update?"); I(id_pre_antilag_update, "pre_antilag_update"); I(id_update, "update");
	I(id_on_old_set, "on_old="); I(iv_ignore_update, "@ignore_update"); I(iv_allow_screen_ignore, "@allow_screen_ignore");
	I(iv_force_update, "@force_update"); I(iv_move_route_forcing, "@move_route_forcing");
	I(iv_character_sprites, "@character_sprites"); I(iv_character, "@character"); I(iv_on_old, "@on_old");
	I(iv_on_new, "@on_new");
	rb_define_global_function("mkxpz_fast_install_antilag_loops", fast_install_antilag_loops, 2);
	I(id_slow_update_events, "mkxpz_slow_update_events"); I(iv_events, "@events"); I(iv_common_events, "@common_events");
	I(id_events_direct_ok, "mkxpz_events_direct_ok?"); I(id_slow_setup_starting, "mkxpz_slow_setup_starting_event");
	I(id_setup_starting_rest, "mkxpz_setup_starting_rest"); I(id_start_event, "mkxpz_start_event");
	I(iv_need_refresh, "@need_refresh"); I(iv_starting, "@starting"); I(gv_game_temp, "$game_temp");
	I(iv_common_event_id, "@common_event_id"); I(id_refresh, "refresh");
	rb_define_global_function("mkxpz_fast_install_vx_map_loops", fast_install_vx_map_loops, 5);
#undef I
}
