#include <oniguruma.h> // Must be first.

#include "builtin/regexp.hpp"
#include "builtin/class.hpp"
#include "builtin/integer.hpp"
#include "builtin/lookuptable.hpp"
#include "builtin/string.hpp"
#include "builtin/symbol.hpp"
#include "builtin/tuple.hpp"
#include "builtin/bytearray.hpp"

#include "vm.hpp"
#include "vm/object_utils.hpp"
#include "objectmemory.hpp"

#include "gc/gc.hpp"

#define OPTION_IGNORECASE ONIG_OPTION_IGNORECASE
#define OPTION_EXTENDED   ONIG_OPTION_EXTEND
#define OPTION_MULTILINE  ONIG_OPTION_MULTILINE
#define OPTION_MASK       (OPTION_IGNORECASE|OPTION_EXTENDED|OPTION_MULTILINE)

#define KCODE_ASCII       0
#define KCODE_NONE        16
#define KCODE_EUC         32
#define KCODE_SJIS        48
#define KCODE_UTF8        64
#define KCODE_MASK        (KCODE_EUC|KCODE_SJIS|KCODE_UTF8)

namespace rubinius {

  void Regexp::init(STATE) {
    onig_init();
    GO(regexp).set(state->new_class("Regexp", G(object), 0));
    G(regexp)->set_object_type(state, RegexpType);

    GO(matchdata).set(state->new_class("MatchData", G(object), 0));
    G(matchdata)->set_object_type(state, MatchDataType);
  }

  char *Regexp::version(STATE) {
    return (char*)onig_version();
  }

  static OnigEncoding get_enc_from_kcode(int kcode) {
    OnigEncoding r;

    r = ONIG_ENCODING_ASCII;
    switch (kcode) {
      case KCODE_NONE:
        r = ONIG_ENCODING_ASCII;
        break;
      case KCODE_EUC:
        r = ONIG_ENCODING_EUC_JP;
        break;
      case KCODE_SJIS:
        r = ONIG_ENCODING_SJIS;
        break;
      case KCODE_UTF8:
        r = ONIG_ENCODING_UTF8;
        break;
    }
    return r;
  }

  int get_kcode_from_enc(OnigEncoding enc) {
    int r;

    r = KCODE_ASCII;
    if (enc == ONIG_ENCODING_ASCII)  r = KCODE_NONE;
    if (enc == ONIG_ENCODING_EUC_JP) r = KCODE_EUC;
    if (enc == ONIG_ENCODING_SJIS)   r = KCODE_SJIS;
    if (enc == ONIG_ENCODING_UTF8)   r = KCODE_UTF8;
    return r;
  }

  struct _gather_data {
    STATE;
    LookupTable* tbl;
  };

  static int _gather_names(const UChar *name, const UChar *name_end,
      int ngroup_num, int *group_nums, regex_t *reg, struct _gather_data *gd) {

    int gn;
    STATE;
    LookupTable* tbl = gd->tbl;

    state = gd->state;

    gn = group_nums[0];
    tbl->store(state, state->symbol((char*)name), Integer::from(state, gn - 1));
    return 0;
  }

  /*
   * Only initialize the object, not oniguruma.  This allows copying of the
   * regular expression via Regexp#initialize_copy
   */
  Regexp* Regexp::create(STATE) {
    Regexp* o_reg = state->new_object_mature<Regexp>(G(regexp));

    o_reg->onig_data = NULL;

    return o_reg;
  }

  void Regexp::make_managed(STATE) {
    Regexp* obj = this;
    regex_t* reg = onig_data;

    assert(reg->chain == 0);

    ByteArray* reg_ba = ByteArray::create(state, sizeof(regex_t));
    memcpy(reg_ba->bytes, reg, sizeof(regex_t));

    regex_t* old_reg = reg;
    reg = reinterpret_cast<regex_t*>(reg_ba->bytes);

    obj->onig_data = reg;
    write_barrier(state, reg_ba);

    if(reg->p) {
      ByteArray* pattern = ByteArray::create(state, reg->alloc);
      memcpy(pattern->bytes, reg->p, reg->alloc);

      reg->p = reinterpret_cast<unsigned char*>(pattern->bytes);

      obj->write_barrier(state, pattern);
    }

    if(reg->exact) {
      int exact_size = reg->exact_end - reg->exact;
      ByteArray* exact = ByteArray::create(state, exact_size);
      memcpy(exact->bytes, reg->exact, exact_size);

      reg->exact = reinterpret_cast<unsigned char*>(exact->bytes);
      reg->exact_end = reg->exact + exact_size;

      obj->write_barrier(state, exact);
    }

    int int_map_size = sizeof(int) * ONIG_CHAR_TABLE_SIZE;

    if(reg->int_map) {
      ByteArray* intmap = ByteArray::create(state, int_map_size);
      memcpy(intmap->bytes, reg->int_map, int_map_size);

      reg->int_map = reinterpret_cast<int*>(intmap->bytes);

      obj->write_barrier(state, intmap);
    }

    if(reg->int_map_backward) {
      ByteArray* intmap_back = ByteArray::create(state, int_map_size);
      memcpy(intmap_back->bytes, reg->int_map_backward, int_map_size);

      reg->int_map_backward = reinterpret_cast<int*>(intmap_back->bytes);

      obj->write_barrier(state, intmap_back);
    }

    if(reg->repeat_range) {
      int rrange_size = sizeof(OnigRepeatRange) * reg->repeat_range_alloc;
      ByteArray* rrange = ByteArray::create(state, rrange_size);
      memcpy(rrange->bytes, reg->repeat_range, rrange_size);

      reg->repeat_range = reinterpret_cast<OnigRepeatRange*>(rrange->bytes);

      obj->write_barrier(state, rrange);
    }

    onig_free(old_reg);
  }

  /*
   * This is a primitive so #initialize_copy can work.
   */
  Regexp* Regexp::initialize(STATE, String* pattern, Integer* options,
                             Object* lang) {
    const UChar *pat;
    const UChar *end;
    OnigErrorInfo err_info;
    OnigOptionType opts;
    OnigEncoding enc;
    int err, num_names, kcode;

    pat = (UChar*)pattern->c_str();
    end = pat + pattern->size();

    opts  = options->to_native();
    kcode = opts & KCODE_MASK;
    enc   = get_enc_from_kcode(kcode);
    opts &= OPTION_MASK;

    err = onig_new(&this->onig_data, pat, end, opts, enc, ONIG_SYNTAX_RUBY, &err_info);

    if(err != ONIG_NORMAL) {
      UChar onig_err_buf[ONIG_MAX_ERROR_MESSAGE_LEN];
      char err_buf[1024];
      onig_error_code_to_str(onig_err_buf, err, &err_info);
      snprintf(err_buf, 1024, "%s: %s", onig_err_buf, pat);

      Exception::regexp_error(state, err_buf);
      return 0;
    }

    this->source(state, pattern);

    num_names = onig_number_of_names(this->onig_data);

    if(num_names == 0) {
      this->names(state, (LookupTable*)Qnil);
    } else {
      struct _gather_data gd;
      gd.state = state;
      LookupTable* tbl = LookupTable::create(state);
      gd.tbl = tbl;
      onig_foreach_name(this->onig_data, (int (*)(const OnigUChar*, const OnigUChar*,int,int*,OnigRegex,void*))_gather_names, (void*)&gd);
      this->names(state, tbl);
    }

    make_managed(state);

    return this;
  }

  // 'self' is passed in automatically by the primitive glue
  Regexp* Regexp::allocate(STATE, Object* self) {
    Regexp* re = Regexp::create(state);
    re->klass(state, (Class*)self);
    return re;
  }

  Object* Regexp::options(STATE) {
    OnigEncoding   enc;
    OnigOptionType option;
    regex_t*       reg;

    reg    = onig_data;
    option = onig_get_options(reg);
    enc    = onig_get_encoding(reg);

    return Integer::from(state, ((int)(option & OPTION_MASK) | get_kcode_from_enc(enc)));
  }

  static Tuple* _md_region_to_tuple(STATE, OnigRegion *region, int max) {
    int i;
    Tuple* sub;
    Tuple* tup = Tuple::create(state, region->num_regs - 1);
    for(i = 1; i < region->num_regs; i++) {
      sub = Tuple::from(state, 2,
			Integer::from(state, region->beg[i]),
			Integer::from(state, region->end[i]));
      tup->put(state, i - 1, sub);
    }
    return tup;
  }

  static Object* get_match_data(STATE, OnigRegion *region, String* string, Regexp* regexp, int max) {
    MatchData* md = state->new_object<MatchData>(G(matchdata));
    md->source(state, string->string_dup(state));
    md->regexp(state, regexp);
    Tuple* tup = Tuple::from(state, 2,
			     Integer::from(state, region->beg[0]),
			     Integer::from(state, region->end[0]));
    md->full(state, tup);
    md->region(state, _md_region_to_tuple(state, region, max));
    return md;
  }

  Object* Regexp::match_region(STATE, String* string, Integer* start,
                               Integer* end, Object* forward)
  {
    int beg, max;
    const UChar *str;
    OnigRegion *region;
    Object* md;

    region = onig_region_new();

    max = string->size();
    str = (UChar*)string->c_str();

    int* back_match = onig_data->int_map_backward;

    if(!RTEST(forward)) {
      beg = onig_search(onig_data, str, str + max,
                        str + end->to_native(),
                        str + start->to_native(),
                        region, ONIG_OPTION_NONE);
    } else {
      beg = onig_search(onig_data, str, str + max,
                        str + start->to_native(),
                        str + end->to_native(),
                        region, ONIG_OPTION_NONE);
    }

    // Seems like onig must setup int_map_backward lazily, so we have to watch
    // for it to appear here.
    if(onig_data->int_map_backward != back_match) {
      size_t size = sizeof(int) * ONIG_CHAR_TABLE_SIZE;
      ByteArray* ba = ByteArray::create(state, size);
      memcpy(ba->bytes, onig_data->int_map_backward, size);

      // Dispose of the old one.
      free(onig_data->int_map_backward);

      onig_data->int_map_backward = reinterpret_cast<int*>(ba->bytes);

      write_barrier(state, ba);
    }


    if(beg == ONIG_MISMATCH) {
      onig_region_free(region, 1);
      return Qnil;
    }

    md = get_match_data(state, region, string, this, max);
    onig_region_free(region, 1);
    return md;
  }

  Object* Regexp::match_start(STATE, String* string, Integer* start) {
    int beg, max;
    const UChar *str;
    OnigRegion *region;
    Object* md = Qnil;

    region = onig_region_new();

    max = string->size();
    str = (UChar*)string->c_str();

    int* back_match = onig_data->int_map_backward;

    beg = onig_match(onig_data, str, str + max, str + start->to_native(), region,
                     ONIG_OPTION_NONE);

    // Seems like onig must setup int_map_backward lazily, so we have to watch
    // for it to appear here.
    if(onig_data->int_map_backward != back_match) {
      size_t size = sizeof(int) * ONIG_CHAR_TABLE_SIZE;
      ByteArray* ba = ByteArray::create(state, size);
      memcpy(ba->bytes, onig_data->int_map_backward, size);

      // Dispose of the old one.
      free(onig_data->int_map_backward);

      onig_data->int_map_backward = reinterpret_cast<int*>(ba->bytes);

      write_barrier(state, ba);
    }

    if(beg != ONIG_MISMATCH) {
      md = get_match_data(state, region, string, this, max);
    }

    onig_region_free(region, 1);
    return md;
  }

  void Regexp::Info::mark(Object* obj, ObjectMark& mark) {
    auto_mark(obj, mark);

    Regexp* reg_o = force_as<Regexp>(obj);
    regex_t* reg = reg_o->onig_data;

    ByteArray* reg_ba = ByteArray::from_body(reg);

    if(ByteArray* reg_tmp = force_as<ByteArray>(mark.call(reg_ba))) {
      reg_o->onig_data = reinterpret_cast<regex_t*>(reg_tmp->bytes);
      mark.just_set(obj, reg_tmp);

      reg_ba = reg_tmp;
      reg = reg_o->onig_data;
    }

    if(reg->p) {
      ByteArray* ba = ByteArray::from_body(reg->p);

      ByteArray* tmp = force_as<ByteArray>(mark.call(ba));
      if(tmp) {
        reg->p = reinterpret_cast<unsigned char*>(tmp->bytes);
        mark.just_set(obj, tmp);
      }
    }

    if(reg->exact) {
      int exact_size = reg->exact_end - reg->exact;
      ByteArray* ba = ByteArray::from_body(reg->exact);

      ByteArray* tmp = force_as<ByteArray>(mark.call(ba));
      if(tmp) {
        reg->exact = reinterpret_cast<unsigned char*>(tmp->bytes);
        reg->exact_end = reg->exact + exact_size;
        mark.just_set(obj, tmp);
      }
    }

    if(reg->int_map) {
      ByteArray* ba = ByteArray::from_body(reg->int_map);

      ByteArray* tmp = force_as<ByteArray>(mark.call(ba));
      if(tmp) {
        reg->int_map = reinterpret_cast<int*>(tmp->bytes);
        mark.just_set(obj, tmp);
      }
    }

    if(reg->int_map_backward) {
      ByteArray* ba = ByteArray::from_body(reg->int_map_backward);

      ByteArray* tmp = force_as<ByteArray>(mark.call(ba));
      if(tmp) {
        reg->int_map_backward = reinterpret_cast<int*>(tmp->bytes);
        mark.just_set(obj, tmp);
      }
    }

    if(reg->repeat_range) {
      ByteArray* ba = ByteArray::from_body(reg->repeat_range);

      ByteArray* tmp = force_as<ByteArray>(mark.call(ba));
      if(tmp) {
        reg->repeat_range = reinterpret_cast<OnigRepeatRange*>(tmp->bytes);
        mark.just_set(obj, tmp);
      }
    }
  }

  void Regexp::Info::visit(Object* obj, ObjectVisitor& visit) {
    auto_visit(obj, visit);

    Regexp* reg_o = force_as<Regexp>(obj);
    regex_t* reg = reg_o->onig_data;

    ByteArray* reg_ba = ByteArray::from_body(reg);
    visit.call(reg_ba);

    if(reg->p) {
      ByteArray* ba = ByteArray::from_body(reg->p);
      visit.call(ba);
    }

    if(reg->exact) {
      ByteArray* ba = ByteArray::from_body(reg->exact);
      visit.call(ba);
    }

    if(reg->int_map) {
      ByteArray* ba = ByteArray::from_body(reg->int_map);
      visit.call(ba);
    }

    if(reg->int_map_backward) {
      ByteArray* ba = ByteArray::from_body(reg->int_map_backward);
      visit.call(ba);
    }

    if(reg->repeat_range) {
      ByteArray* ba = ByteArray::from_body(reg->repeat_range);
      visit.call(ba);
    }
  }
}
