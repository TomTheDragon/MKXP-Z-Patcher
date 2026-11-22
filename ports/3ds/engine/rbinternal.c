/* The few Ruby 3.1 internals the engine reads, compiled with Ruby's own
 * internal headers (which the other files don't include). */
#include "ruby/ruby.h"
#include "vm_core.h"
#include "id.h"
#include "internal/class.h"
#include "internal/variable.h"
#include "variable.h"
#include "engine.h"

/* An instance variable without Ruby's hash lookup by name: each call site
 * remembers the class and the variable's slot (a class's slots never move
 * in Ruby 3.1; a slot past the object's end or never set reads as nil, as
 * in Ruby). Plain objects (Game_Event, Game_Map...) and the engine's C
 * objects (Sprite_Character's variables, in Ruby's table for those). */
VALUE mkxpz_ivar(VALUE obj, ID id, IvCache *c)
{
	if (RB_SPECIAL_CONST_P(obj))
		return rb_ivar_get(obj, id);
	int type = RB_BUILTIN_TYPE(obj);
	if (type != T_OBJECT && type != T_DATA)
		return rb_ivar_get(obj, id);
	VALUE klass = RBASIC(obj)->klass;
	if (c->klass != klass) {
		st_table *t = RCLASS_IV_INDEX_TBL(rb_obj_class(obj));
		st_data_t ent;
		if (!t || !st_lookup(t, (st_data_t) id, &ent))
			return rb_ivar_get(obj, id); /* (not in the class yet) */
		c->klass = klass;
		c->index = ((struct rb_iv_index_tbl_entry *) ent)->index;
	}
	VALUE v;
	if (type == T_OBJECT) {
		if (c->index >= ROBJECT_NUMIV(obj))
			return Qnil;
		v = ROBJECT_IVPTR(obj)[c->index];
	} else {
		struct gen_ivtbl *ivtbl;
		if (!FL_TEST_RAW(obj, FL_EXIVAR) || !rb_ivar_generic_ivtbl_lookup(obj, &ivtbl) || c->index >= ivtbl->numiv)
			return Qnil;
		v = ivtbl->ivptr[c->index];
	}
	return v == Qundef ? Qnil : v;
}

/* setting a plain object's instance variable in its slot (with the write
 * barrier); anything else (a new variable, frozen, C objects): rb_ivar_set */
void mkxpz_ivar_set(VALUE obj, ID id, VALUE val, IvCache *c)
{
	if (RB_SPECIAL_CONST_P(obj) || RB_BUILTIN_TYPE(obj) != T_OBJECT || RB_OBJ_FROZEN_RAW(obj)) {
		rb_ivar_set(obj, id, val);
		return;
	}
	VALUE klass = RBASIC(obj)->klass;
	if (c->klass != klass) {
		st_table *t = RCLASS_IV_INDEX_TBL(rb_obj_class(obj));
		st_data_t ent;
		if (!t || !st_lookup(t, (st_data_t) id, &ent)) {
			rb_ivar_set(obj, id, val);
			return;
		}
		c->klass = klass;
		c->index = ((struct rb_iv_index_tbl_entry *) ent)->index;
	}
	if (c->index >= ROBJECT_NUMIV(obj)) {
		rb_ivar_set(obj, id, val);
		return;
	}
	RB_OBJ_WRITE(obj, &ROBJECT_IVPTR(obj)[c->index], val);
}

/* the class a profiler frame (from rb_profile_frames) belongs to, without
 * allocating anything (rb_profile_frame_classpath copies the name, and the
 * profiler's job must not make Ruby objects: see profile.c): its name
 * string (or nil: anonymous), singleton: a class method */
VALUE mkxpz_frame_class_name(VALUE frame, bool *singleton)
{
	*singleton = false;
	if (!RB_TYPE_P(frame, T_IMEMO) || imemo_type(frame) != imemo_ment)
		return Qnil;
	VALUE klass = ((const rb_callable_method_entry_t *) frame)->defined_class;
	if (!klass || RB_SPECIAL_CONST_P(klass))
		return Qnil;
	if (RB_TYPE_P(klass, T_ICLASS))
		klass = RBASIC(klass)->klass;
	if (!klass || RB_SPECIAL_CONST_P(klass))
		return Qnil;
	if (FL_TEST(klass, FL_SINGLETON)) {
		*singleton = true;
		klass = rb_attr_get(klass, id__attached__);
		if (!RB_TYPE_P(klass, T_CLASS) && !RB_TYPE_P(klass, T_MODULE))
			return Qnil;
	}
	if (!RB_TYPE_P(klass, T_CLASS) && !RB_TYPE_P(klass, T_MODULE))
		return Qnil;
	return rb_mod_name(klass);
}

/* a global variable by ID (rb_gv_get looks the name up each time) */
VALUE mkxpz_gvar(ID id)
{
	return rb_gvar_get(id);
}
