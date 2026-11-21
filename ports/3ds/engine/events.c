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
#undef I
	rb_define_global_function("mkxpz_fast_install_events", fast_install_events, 1);
}
