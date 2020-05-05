/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#ifndef incl_HPHP_JIT_IRGEN_MINSTR_H_
#define incl_HPHP_JIT_IRGEN_MINSTR_H_

#include "hphp/runtime/base/static-string-table.h"

#include "hphp/runtime/vm/jit/extra-data.h"
#include "hphp/runtime/vm/jit/guard-constraint.h"
#include "hphp/runtime/vm/jit/ir-builder.h"
#include "hphp/runtime/vm/jit/irgen.h"
#include "hphp/runtime/vm/jit/irgen-exit.h"
#include "hphp/runtime/vm/jit/irgen-internal.h"
#include "hphp/runtime/vm/jit/irgen-state.h"
#include "hphp/runtime/vm/jit/array-access-profile.h"
#include "hphp/runtime/vm/jit/ssa-tmp.h"
#include "hphp/runtime/vm/jit/target-profile.h"
#include "hphp/runtime/vm/jit/type-profile.h"

namespace HPHP { namespace jit { namespace irgen {

///////////////////////////////////////////////////////////////////////////////

/*
 * Returns true if the given property may have a countable type. This check
 * is allowed to have false positives; in particular: if property type-hint
 * enforcement is disabled, it will usually return true. (It may still return
 * false in RepoAuthoritative mode if HHBBC can prove a property is uncounted.)
 *
 * It is safe to call this method during Class initialization.
 */
bool propertyMayBeCountable(const Class::Prop& prop);

///////////////////////////////////////////////////////////////////////////////

void logArrayAccessProfile(IRGS& env, SSATmp* arr, SSATmp* key,
                           MOpMode mode, const ArrayAccessProfile& profile);

/*
 * Use profiling data from an ArrayAccessProfile to conditionally optimize
 * the array access represented by `generic' using `direct' or `missing`.
 *
 * For profiling translations, we generate code that updates the profile, then
 * falls back to `generic'. In optimized translations:
 *
 *  - If the key is likely to be at a particular offset in the array-like, we
 *    generate a Check(MixedArray|Dict|Keyset)Offset. If it passes, we use
 *    `direct`, else we fall back to `generic`.
 *
 *  - If the key is likely to be missing in some way that we can quickly check,
 *    we do so and then call `missing`, else we fall back to `generic`.
 *
 *  - If no optimized access is possible, we just use `generic`.
 *
 * When we call `generic`, if we're optimizing, we'll pass it SizeHintData
 * that can be used to optimize generic lookups.
 *
 * The callback function signatures should be:
 *
 *    SSATmp* direct (SSATmp* key, uint32_t pos);
 *    SSATmp* missing(SSATmp* key);
 *    SSATmp* generic(SSATmp* key, SizeHintData data);
 */
template<class Direct, class Missing, class Generic>
SSATmp* profiledArrayAccess(IRGS& env, SSATmp* arr, SSATmp* key, MOpMode mode,
                            Direct direct, Missing missing, Generic generic) {
  // These locals should be const, but we need to work around a bug in older
  // versions of GCC that cause the hhvm-cmake build to fail. See the issue:
  // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=80543
  bool is_dict = arr->isA(TDict);
  bool is_keyset = arr->isA(TKeyset);
  bool is_define = mode == MOpMode::Define;
  bool cow_check = mode == MOpMode::Define || mode == MOpMode::Unset;
  assertx(is_dict || is_keyset || arr->isA(TArr));

  // If the access is statically known, don't bother profiling as we'll probably
  // optimize it away completely.
  if (arr->hasConstVal() && key->hasConstVal()) {
    return generic(key, SizeHintData{});
  }

  static const StaticString s_DictAccess{"DictAccess"};
  static const StaticString s_KeysetAccess{"KeysetAccess"};
  static const StaticString s_MixedArrayAccess{"MixedArrayAccess"};
  auto const profile = TargetProfile<ArrayAccessProfile> {
    env.unit,
    env.irb->curMarker(),
    is_dict ? s_DictAccess.get() :
    is_keyset ? s_KeysetAccess.get() :
    s_MixedArrayAccess.get()
  };

  if (profile.profiling()) {
    auto const op = is_dict ? ProfileDictAccess
                            : is_keyset ? ProfileKeysetAccess
                                        : ProfileMixedArrayAccess;
    auto const data = ArrayAccessProfileData { profile.handle(), cow_check };
    gen(env, op, data, arr, key);
    return generic(key, SizeHintData{});
  }

  if (profile.optimizing()) {
    auto const data = profile.data();
    auto const result = data.choose();
    logArrayAccessProfile(env, arr, key, mode, data);

    FTRACE_MOD(Trace::idx, 1, "{}\nArrayAccessProfile: {}\n",
               env.irb->curMarker().show(), data.toString());

    using MKAction = ArrayAccessProfile::MissingKeyAction;

    auto const missingCond = [&] (MKAction action, auto f) {
      assertx(action != MKAction::None);
      return cond(env,
        f,
        [&] {
          return missing(key);
        },
        [&] {
          hint(env, Block::Hint::Unlikely);
          if (action == MKAction::Cold) return generic(key, result.size_hint);
          gen(env, Jmp, makeExitSlow(env));
          return cns(env, TBottom);
        }
      );
    };

    if (!is_define && result.empty != MKAction::None) {
      return missingCond(result.empty, [&] (Block* taken) {
        auto const count = [&] {
          if (is_dict) return gen(env, CountDict, arr);
          if (is_keyset) return gen(env, CountKeyset, arr);
          return gen(env, CountArrayFast, arr);
        }();
        gen(env, JmpNZero, taken, count);
      });
    }
    if (!is_define && !is_keyset && result.missing != MKAction::None) {
      return missingCond(result.missing, [&] (Block* taken) {
        // According to the profiling, the key is mostly a TStaticStr.
        // If if the JIT doesn't know that statically, lets check for it.
        auto const skey = key->isA(TStaticStr) ? key :
          gen(env, CheckType, TStaticStr, taken, key);
        gen(env, CheckMissingKeyInArrLike, taken, arr, skey);
        auto const t = is_dict ? TStaticDict :
                       is_keyset ? TStaticKeyset : TStaticArr;
        gen(env, AssertType, t, arr);
      });
    }
    if (!result.offset) return generic(key, result.size_hint);

    return cond(
      env,
      [&] (Block* taken) {
        auto const marr = [&](){
          if (is_dict || is_keyset) return arr;
          return gen(env, CheckType, TMixedArr, taken, arr);
        }();

        auto const op = is_dict ? CheckDictOffset
                                : is_keyset ? CheckKeysetOffset
                                            : CheckMixedArrayOffset;
        gen(env, op, IndexData { *result.offset }, taken, marr, key);
        if (cow_check) gen(env, CheckArrayCOW, taken, marr);
        return marr;
      },
      [&] (SSATmp* tmp) { return direct(tmp, key, *result.offset); },
      [&] {
        // NOTE: We could pass result.size_hint here, but that's the size hint
        // for the overall distribution, not the conditional distribution when
        // the likely offset profile misses. We pass a default profile instead.
        hint(env, Block::Hint::Unlikely);
        return generic(key, SizeHintData{});
      }
    );
  }
  return generic(key, SizeHintData{});
}

///////////////////////////////////////////////////////////////////////////////

/*
 * Use TypeProfile to profile the type of `tmp' (typically loaded from
 * the heap) and emit a type check in optimizing translations to
 * refine some properties of the types observed during profiling.
 * Such refinements include checking a specific type in case it's
 * monomorphic, or checking that it's uncounted.  In case
 * the check fails dynamically, a side exit is taken.  The `finish'
 * lambda is invoked to emit code before exiting the region at the
 * next bytecode-instruction boundary.
 */
template<class Finish>
SSATmp* profiledType(IRGS& env, SSATmp* tmp, Finish finish) {
  if (tmp->type() <= TCell && tmp->type().isKnownDataType()) {
    return tmp;
  }

  static const StaticString s_TypeProfile{"TypeProfile"};
  TargetProfile<TypeProfile> prof(env.unit, env.irb->curMarker(),
                                  s_TypeProfile.get());

  if (prof.profiling()) {
    gen(env, ProfileType, RDSHandleData{ prof.handle() }, tmp);
  }

  if (!prof.optimizing()) return tmp;

  auto const reducedType = prof.data().type;

  if (reducedType == TBottom) {
    // We got no samples
    return tmp;
  }

  Type typeToCheck = relaxToGuardable(reducedType);

  if (typeToCheck == TCell) return tmp;

  SSATmp* ptmp{nullptr};

  ifThen(env,
         [&](Block* taken) {
           ptmp = gen(env, CheckType, typeToCheck, taken, tmp);
         },
         [&] {
           hint(env, Block::Hint::Unlikely);
           auto const takenType = negativeCheckType(tmp->type(), typeToCheck);
           if (takenType < tmp->type()) {
             gen(env, AssertType, takenType, tmp);
           }
           finish();
           gen(env, Jmp, makeExit(env, nextBcOff(env)));
         });

  return ptmp;
}

///////////////////////////////////////////////////////////////////////////////

}}}

#endif
