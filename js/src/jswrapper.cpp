/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jswrapper.h"

#include "jscntxt.h"
#include "jscompartment.h"
#include "jsexn.h"
#include "jsgc.h"
#include "jsiter.h"

#include "vm/ErrorObject.h"
#include "vm/WrapperObject.h"

#include "jsobjinlines.h"

using namespace js;
using namespace js::gc;

/*
 * Wrapper forwards this call directly to the wrapped object for efficiency
 * and transparency. In particular, the hint is needed to properly stringify
 * Date objects in certain cases - see bug 646129. Note also the
 * SecurityWrapper overrides this trap to avoid information leaks. See bug
 * 720619.
 */
bool
Wrapper::defaultValue(JSContext *cx, HandleObject proxy, JSType hint, MutableHandleValue vp) const
{
    vp.set(ObjectValue(*proxy->as<ProxyObject>().target()));
    if (hint == JSTYPE_VOID)
        return ToPrimitive(cx, vp);
    return ToPrimitive(cx, hint, vp);
}

JSObject *
Wrapper::New(JSContext *cx, JSObject *obj, JSObject *parent, const Wrapper *handler,
             const WrapperOptions *options)
{
    JS_ASSERT(parent);

    RootedValue priv(cx, ObjectValue(*obj));
    mozilla::Maybe<WrapperOptions> opts;
    if (!options) {
        opts.emplace();
        opts->selectDefaultClass(obj->isCallable());
        options = opts.ptr();
    }
    return NewProxyObject(cx, handler, priv, options->proto(), parent, *options);
}

JSObject *
Wrapper::Renew(JSContext *cx, JSObject *existing, JSObject *obj, const Wrapper *handler)
{
    JS_ASSERT(!obj->isCallable());
    existing->as<ProxyObject>().renew(cx, handler, ObjectValue(*obj));
    return existing;
}

const Wrapper *
Wrapper::wrapperHandler(JSObject *wrapper)
{
    JS_ASSERT(wrapper->is<WrapperObject>());
    return static_cast<const Wrapper*>(wrapper->as<ProxyObject>().handler());
}

JSObject *
Wrapper::wrappedObject(JSObject *wrapper)
{
    JS_ASSERT(wrapper->is<WrapperObject>());
    return wrapper->as<ProxyObject>().target();
}

JS_FRIEND_API(JSObject *)
js::UncheckedUnwrap(JSObject *wrapped, bool stopAtOuter, unsigned *flagsp)
{
    unsigned flags = 0;
    while (true) {
        if (!wrapped->is<WrapperObject>() ||
            MOZ_UNLIKELY(stopAtOuter && wrapped->getClass()->ext.innerObject))
        {
            break;
        }
        flags |= Wrapper::wrapperHandler(wrapped)->flags();
        wrapped = wrapped->as<ProxyObject>().private_().toObjectOrNull();

        // This can be called from DirectProxyHandler::weakmapKeyDelegate() on a
        // wrapper whose referent has been moved while it is still unmarked.
        if (wrapped)
            wrapped = MaybeForwarded(wrapped);
    }
    if (flagsp)
        *flagsp = flags;
    return wrapped;
}

JS_FRIEND_API(JSObject *)
js::CheckedUnwrap(JSObject *obj, bool stopAtOuter)
{
    while (true) {
        JSObject *wrapper = obj;
        obj = UnwrapOneChecked(obj, stopAtOuter);
        if (!obj || obj == wrapper)
            return obj;
    }
}

JS_FRIEND_API(JSObject *)
js::UnwrapOneChecked(JSObject *obj, bool stopAtOuter)
{
    if (!obj->is<WrapperObject>() ||
        MOZ_UNLIKELY(!!obj->getClass()->ext.innerObject && stopAtOuter))
    {
        return obj;
    }

    const Wrapper *handler = Wrapper::wrapperHandler(obj);
    return handler->hasSecurityPolicy() ? nullptr : Wrapper::wrappedObject(obj);
}

bool
js::IsCrossCompartmentWrapper(JSObject *obj)
{
    return IsWrapper(obj) &&
           !!(Wrapper::wrapperHandler(obj)->flags() & Wrapper::CROSS_COMPARTMENT);
}

const char Wrapper::family = 0;
const Wrapper Wrapper::singleton((unsigned)0);
const Wrapper Wrapper::singletonWithPrototype((unsigned)0, true);
JSObject *Wrapper::defaultProto = TaggedProto::LazyProto;

/* Compartments. */

extern JSObject *
js::TransparentObjectWrapper(JSContext *cx, HandleObject existing, HandleObject obj,
                             HandleObject parent)
{
    // Allow wrapping outer window proxies.
    JS_ASSERT(!obj->is<WrapperObject>() || obj->getClass()->ext.innerObject);
    return Wrapper::New(cx, obj, parent, &CrossCompartmentWrapper::singleton);
}

ErrorCopier::~ErrorCopier()
{
    JSContext *cx = ac->context()->asJSContext();
    if (ac->origin() != cx->compartment() && cx->isExceptionPending()) {
        RootedValue exc(cx);
        if (cx->getPendingException(&exc) && exc.isObject() && exc.toObject().is<ErrorObject>()) {
            cx->clearPendingException();
            ac.reset();
            Rooted<ErrorObject*> errObj(cx, &exc.toObject().as<ErrorObject>());
            JSObject *copyobj = js_CopyErrorObject(cx, errObj);
            if (copyobj)
                cx->setPendingException(ObjectValue(*copyobj));
        }
    }
}

/* Cross compartment wrappers. */

bool Wrapper::finalizeInBackground(Value priv) const
{
    if (!priv.isObject())
        return true;

    /*
     * Make the 'background-finalized-ness' of the wrapper the same as the
     * wrapped object, to allow transplanting between them.
     *
     * If the wrapped object is in the nursery then we know it doesn't have a
     * finalizer, and so background finalization is ok.
     */
    if (IsInsideNursery(&priv.toObject()))
        return true;
    return IsBackgroundFinalized(priv.toObject().tenuredGetAllocKind());
}

#define PIERCE(cx, wrapper, pre, op, post)                      \
    JS_BEGIN_MACRO                                              \
        bool ok;                                                \
        {                                                       \
            AutoCompartment call(cx, wrappedObject(wrapper));   \
            ok = (pre) && (op);                                 \
        }                                                       \
        return ok && (post);                                    \
    JS_END_MACRO

#define NOTHING (true)

bool
CrossCompartmentWrapper::isExtensible(JSContext *cx, HandleObject wrapper, bool *extensible) const
{
    PIERCE(cx, wrapper,
           NOTHING,
           Wrapper::isExtensible(cx, wrapper, extensible),
           NOTHING);
}

bool
CrossCompartmentWrapper::preventExtensions(JSContext *cx, HandleObject wrapper) const
{
    PIERCE(cx, wrapper,
           NOTHING,
           Wrapper::preventExtensions(cx, wrapper),
           NOTHING);
}

bool
CrossCompartmentWrapper::getPropertyDescriptor(JSContext *cx, HandleObject wrapper, HandleId id,
                                               MutableHandle<PropertyDescriptor> desc) const
{
    PIERCE(cx, wrapper,
           NOTHING,
           Wrapper::getPropertyDescriptor(cx, wrapper, id, desc),
           cx->compartment()->wrap(cx, desc));
}

bool
CrossCompartmentWrapper::getOwnPropertyDescriptor(JSContext *cx, HandleObject wrapper, HandleId id,
                                                  MutableHandle<PropertyDescriptor> desc) const
{
    PIERCE(cx, wrapper,
           NOTHING,
           Wrapper::getOwnPropertyDescriptor(cx, wrapper, id, desc),
           cx->compartment()->wrap(cx, desc));
}

bool
CrossCompartmentWrapper::defineProperty(JSContext *cx, HandleObject wrapper, HandleId id,
                                        MutableHandle<PropertyDescriptor> desc) const
{
    Rooted<PropertyDescriptor> desc2(cx, desc);
    PIERCE(cx, wrapper,
           cx->compartment()->wrap(cx, &desc2),
           Wrapper::defineProperty(cx, wrapper, id, &desc2),
           NOTHING);
}

bool
CrossCompartmentWrapper::getOwnPropertyNames(JSContext *cx, HandleObject wrapper,
                                             AutoIdVector &props) const
{
    PIERCE(cx, wrapper,
           NOTHING,
           Wrapper::getOwnPropertyNames(cx, wrapper, props),
           NOTHING);
}

bool
CrossCompartmentWrapper::delete_(JSContext *cx, HandleObject wrapper, HandleId id, bool *bp) const
{
    PIERCE(cx, wrapper,
           NOTHING,
           Wrapper::delete_(cx, wrapper, id, bp),
           NOTHING);
}

bool
CrossCompartmentWrapper::enumerate(JSContext *cx, HandleObject wrapper, AutoIdVector &props) const
{
    PIERCE(cx, wrapper,
           NOTHING,
           Wrapper::enumerate(cx, wrapper, props),
           NOTHING);
}

bool
CrossCompartmentWrapper::has(JSContext *cx, HandleObject wrapper, HandleId id, bool *bp) const
{
    PIERCE(cx, wrapper,
           NOTHING,
           Wrapper::has(cx, wrapper, id, bp),
           NOTHING);
}

bool
CrossCompartmentWrapper::hasOwn(JSContext *cx, HandleObject wrapper, HandleId id, bool *bp) const
{
    PIERCE(cx, wrapper,
           NOTHING,
           Wrapper::hasOwn(cx, wrapper, id, bp),
           NOTHING);
}

bool
CrossCompartmentWrapper::get(JSContext *cx, HandleObject wrapper, HandleObject receiver,
                             HandleId id, MutableHandleValue vp) const
{
    RootedObject receiverCopy(cx, receiver);
    {
        AutoCompartment call(cx, wrappedObject(wrapper));
        if (!cx->compartment()->wrap(cx, &receiverCopy))
            return false;

        if (!Wrapper::get(cx, wrapper, receiverCopy, id, vp))
            return false;
    }
    return cx->compartment()->wrap(cx, vp);
}

bool
CrossCompartmentWrapper::set(JSContext *cx, HandleObject wrapper, HandleObject receiver,
                             HandleId id, bool strict, MutableHandleValue vp) const
{
    RootedObject receiverCopy(cx, receiver);
    PIERCE(cx, wrapper,
           cx->compartment()->wrap(cx, &receiverCopy) &&
           cx->compartment()->wrap(cx, vp),
           Wrapper::set(cx, wrapper, receiverCopy, id, strict, vp),
           NOTHING);
}

bool
CrossCompartmentWrapper::keys(JSContext *cx, HandleObject wrapper, AutoIdVector &props) const
{
    PIERCE(cx, wrapper,
           NOTHING,
           Wrapper::keys(cx, wrapper, props),
           NOTHING);
}

/*
 * We can reify non-escaping iterator objects instead of having to wrap them. This
 * allows fast iteration over objects across a compartment boundary.
 */
static bool
CanReify(HandleValue vp)
{
    JSObject *obj;
    return vp.isObject() &&
           (obj = &vp.toObject())->is<PropertyIteratorObject>() &&
           (obj->as<PropertyIteratorObject>().getNativeIterator()->flags & JSITER_ENUMERATE);
}

struct AutoCloseIterator
{
    AutoCloseIterator(JSContext *cx, JSObject *obj) : cx(cx), obj(cx, obj) {}

    ~AutoCloseIterator() { if (obj) CloseIterator(cx, obj); }

    void clear() { obj = nullptr; }

  private:
    JSContext *cx;
    RootedObject obj;
};

static bool
Reify(JSContext *cx, JSCompartment *origin, MutableHandleValue vp)
{
    Rooted<PropertyIteratorObject*> iterObj(cx, &vp.toObject().as<PropertyIteratorObject>());
    NativeIterator *ni = iterObj->getNativeIterator();

    AutoCloseIterator close(cx, iterObj);

    /* Wrap the iteratee. */
    RootedObject obj(cx, ni->obj);
    if (!origin->wrap(cx, &obj))
        return false;

    /*
     * Wrap the elements in the iterator's snapshot.
     * N.B. the order of closing/creating iterators is important due to the
     * implicit cx->enumerators state.
     */
    size_t length = ni->numKeys();
    bool isKeyIter = ni->isKeyIter();
    AutoIdVector keys(cx);
    if (length > 0) {
        if (!keys.reserve(length))
            return false;
        for (size_t i = 0; i < length; ++i) {
            RootedId id(cx);
            RootedValue v(cx, StringValue(ni->begin()[i]));
            if (!ValueToId<CanGC>(cx, v, &id))
                return false;
            keys.infallibleAppend(id);
        }
    }

    close.clear();
    if (!CloseIterator(cx, iterObj))
        return false;

    if (isKeyIter) {
        if (!VectorToKeyIterator(cx, obj, ni->flags, keys, vp))
            return false;
    } else {
        if (!VectorToValueIterator(cx, obj, ni->flags, keys, vp))
            return false;
    }
    return true;
}

bool
CrossCompartmentWrapper::iterate(JSContext *cx, HandleObject wrapper, unsigned flags,
                                 MutableHandleValue vp) const
{
    {
        AutoCompartment call(cx, wrappedObject(wrapper));
        if (!Wrapper::iterate(cx, wrapper, flags, vp))
            return false;
    }

    if (CanReify(vp))
        return Reify(cx, cx->compartment(), vp);
    return cx->compartment()->wrap(cx, vp);
}

bool
CrossCompartmentWrapper::call(JSContext *cx, HandleObject wrapper, const CallArgs &args) const
{
    RootedObject wrapped(cx, wrappedObject(wrapper));

    {
        AutoCompartment call(cx, wrapped);

        args.setCallee(ObjectValue(*wrapped));
        if (!cx->compartment()->wrap(cx, args.mutableThisv()))
            return false;

        for (size_t n = 0; n < args.length(); ++n) {
            if (!cx->compartment()->wrap(cx, args[n]))
                return false;
        }

        if (!Wrapper::call(cx, wrapper, args))
            return false;
    }

    return cx->compartment()->wrap(cx, args.rval());
}

bool
CrossCompartmentWrapper::construct(JSContext *cx, HandleObject wrapper, const CallArgs &args) const
{
    RootedObject wrapped(cx, wrappedObject(wrapper));
    {
        AutoCompartment call(cx, wrapped);

        for (size_t n = 0; n < args.length(); ++n) {
            if (!cx->compartment()->wrap(cx, args[n]))
                return false;
        }
        if (!Wrapper::construct(cx, wrapper, args))
            return false;
    }
    return cx->compartment()->wrap(cx, args.rval());
}

bool
CrossCompartmentWrapper::nativeCall(JSContext *cx, IsAcceptableThis test, NativeImpl impl,
                                    CallArgs srcArgs) const
{
    RootedObject wrapper(cx, &srcArgs.thisv().toObject());
    JS_ASSERT(srcArgs.thisv().isMagic(JS_IS_CONSTRUCTING) ||
              !UncheckedUnwrap(wrapper)->is<CrossCompartmentWrapperObject>());

    RootedObject wrapped(cx, wrappedObject(wrapper));
    {
        AutoCompartment call(cx, wrapped);
        InvokeArgs dstArgs(cx);
        if (!dstArgs.init(srcArgs.length()))
            return false;

        Value *src = srcArgs.base();
        Value *srcend = srcArgs.array() + srcArgs.length();
        Value *dst = dstArgs.base();

        RootedValue source(cx);
        for (; src < srcend; ++src, ++dst) {
            source = *src;
            if (!cx->compartment()->wrap(cx, &source))
                return false;
            *dst = source.get();

            // Handle |this| specially. When we rewrap on the other side of the
            // membrane, we might apply a same-compartment security wrapper that
            // will stymie this whole process. If that happens, unwrap the wrapper.
            // This logic can go away when same-compartment security wrappers go away.
            if ((src == srcArgs.base() + 1) && dst->isObject()) {
                RootedObject thisObj(cx, &dst->toObject());
                if (thisObj->is<WrapperObject>() &&
                    Wrapper::wrapperHandler(thisObj)->hasSecurityPolicy())
                {
                    JS_ASSERT(!thisObj->is<CrossCompartmentWrapperObject>());
                    *dst = ObjectValue(*Wrapper::wrappedObject(thisObj));
                }
            }
        }

        if (!CallNonGenericMethod(cx, test, impl, dstArgs))
            return false;

        srcArgs.rval().set(dstArgs.rval());
    }
    return cx->compartment()->wrap(cx, srcArgs.rval());
}

bool
CrossCompartmentWrapper::hasInstance(JSContext *cx, HandleObject wrapper, MutableHandleValue v,
                                     bool *bp) const
{
    AutoCompartment call(cx, wrappedObject(wrapper));
    if (!cx->compartment()->wrap(cx, v))
        return false;
    return Wrapper::hasInstance(cx, wrapper, v, bp);
}

const char *
CrossCompartmentWrapper::className(JSContext *cx, HandleObject wrapper) const
{
    AutoCompartment call(cx, wrappedObject(wrapper));
    return Wrapper::className(cx, wrapper);
}

JSString *
CrossCompartmentWrapper::fun_toString(JSContext *cx, HandleObject wrapper, unsigned indent) const
{
    RootedString str(cx);
    {
        AutoCompartment call(cx, wrappedObject(wrapper));
        str = Wrapper::fun_toString(cx, wrapper, indent);
        if (!str)
            return nullptr;
    }
    if (!cx->compartment()->wrap(cx, str.address()))
        return nullptr;
    return str;
}

bool
CrossCompartmentWrapper::regexp_toShared(JSContext *cx, HandleObject wrapper, RegExpGuard *g) const
{
    RegExpGuard wrapperGuard(cx);
    {
        AutoCompartment call(cx, wrappedObject(wrapper));
        if (!Wrapper::regexp_toShared(cx, wrapper, &wrapperGuard))
            return false;
    }

    // Get an equivalent RegExpShared associated with the current compartment.
    RegExpShared *re = wrapperGuard.re();
    return cx->compartment()->regExps.get(cx, re->getSource(), re->getFlags(), g);
}

bool
CrossCompartmentWrapper::boxedValue_unbox(JSContext *cx, HandleObject wrapper, MutableHandleValue vp) const
{
    PIERCE(cx, wrapper,
           NOTHING,
           Wrapper::boxedValue_unbox(cx, wrapper, vp),
           cx->compartment()->wrap(cx, vp));
}

bool
CrossCompartmentWrapper::defaultValue(JSContext *cx, HandleObject wrapper, JSType hint,
                                      MutableHandleValue vp) const
{
    PIERCE(cx, wrapper,
           NOTHING,
           Wrapper::defaultValue(cx, wrapper, hint, vp),
           cx->compartment()->wrap(cx, vp));
}

bool
CrossCompartmentWrapper::getPrototypeOf(JSContext *cx, HandleObject wrapper,
                                        MutableHandleObject protop) const
{
    {
        RootedObject wrapped(cx, wrappedObject(wrapper));
        AutoCompartment call(cx, wrapped);
        if (!JSObject::getProto(cx, wrapped, protop))
            return false;
        if (protop)
            protop->setDelegate(cx);
    }

    return cx->compartment()->wrap(cx, protop);
}

bool
CrossCompartmentWrapper::setPrototypeOf(JSContext *cx, HandleObject wrapper,
                                        HandleObject proto, bool *bp) const
{
    RootedObject protoCopy(cx, proto);
    PIERCE(cx, wrapper,
           cx->compartment()->wrap(cx, &protoCopy),
           Wrapper::setPrototypeOf(cx, wrapper, protoCopy, bp),
           NOTHING);
}

const CrossCompartmentWrapper CrossCompartmentWrapper::singleton(0u);

/* Security wrappers. */

template <class Base>
bool
SecurityWrapper<Base>::isExtensible(JSContext *cx, HandleObject wrapper, bool *extensible) const
{
    // Just like BaseProxyHandler, SecurityWrappers claim by default to always
    // be extensible, so as not to leak information about the state of the
    // underlying wrapped thing.
    *extensible = true;
    return true;
}

template <class Base>
bool
SecurityWrapper<Base>::preventExtensions(JSContext *cx, HandleObject wrapper) const
{
    // See above.
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_UNWRAP_DENIED);
    return false;
}

template <class Base>
bool
SecurityWrapper<Base>::enter(JSContext *cx, HandleObject wrapper, HandleId id,
                             Wrapper::Action act, bool *bp) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_UNWRAP_DENIED);
    *bp = false;
    return false;
}

template <class Base>
bool
SecurityWrapper<Base>::nativeCall(JSContext *cx, IsAcceptableThis test, NativeImpl impl,
                                  CallArgs args) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_UNWRAP_DENIED);
    return false;
}

template <class Base>
bool
SecurityWrapper<Base>::setPrototypeOf(JSContext *cx, HandleObject wrapper,
                                      HandleObject proto, bool *bp) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_UNWRAP_DENIED);
    return false;
}

// For security wrappers, we run the DefaultValue algorithm on the wrapper
// itself, which means that the existing security policy on operations like
// toString() will take effect and do the right thing here.
template <class Base>
bool
SecurityWrapper<Base>::defaultValue(JSContext *cx, HandleObject wrapper,
                                    JSType hint, MutableHandleValue vp) const
{
    return DefaultValue(cx, wrapper, hint, vp);
}

template <class Base>
bool
SecurityWrapper<Base>::objectClassIs(HandleObject obj, ESClassValue classValue, JSContext *cx) const
{
    return false;
}

template <class Base>
bool
SecurityWrapper<Base>::regexp_toShared(JSContext *cx, HandleObject obj, RegExpGuard *g) const
{
    return Base::regexp_toShared(cx, obj, g);
}

template <class Base>
bool
SecurityWrapper<Base>::boxedValue_unbox(JSContext *cx, HandleObject obj, MutableHandleValue vp) const
{
    vp.setUndefined();
    return true;
}

template <class Base>
bool
SecurityWrapper<Base>::defineProperty(JSContext *cx, HandleObject wrapper,
                                      HandleId id, MutableHandle<PropertyDescriptor> desc) const
{
    if (desc.getter() || desc.setter()) {
        JSString *str = IdToString(cx, id);
        AutoStableStringChars chars(cx);
        const jschar *prop = nullptr;
        if (str->ensureFlat(cx) && chars.initTwoByte(cx, str))
            prop = chars.twoByteChars();
        JS_ReportErrorNumberUC(cx, js_GetErrorMessage, nullptr,
                               JSMSG_ACCESSOR_DEF_DENIED, prop);
        return false;
    }

    return Base::defineProperty(cx, wrapper, id, desc);
}

template <class Base>
bool
SecurityWrapper<Base>::watch(JSContext *cx, HandleObject proxy,
                             HandleId id, HandleObject callable) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_UNWRAP_DENIED);
    return false;
}

template <class Base>
bool
SecurityWrapper<Base>::unwatch(JSContext *cx, HandleObject proxy,
                               HandleId id) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_UNWRAP_DENIED);
    return false;
}


template class js::SecurityWrapper<Wrapper>;
template class js::SecurityWrapper<CrossCompartmentWrapper>;

bool
DeadObjectProxy::isExtensible(JSContext *cx, HandleObject proxy, bool *extensible) const
{
    // This is kind of meaningless, but dead-object semantics aside,
    // [[Extensible]] always being true is consistent with other proxy types.
    *extensible = true;
    return true;
}

bool
DeadObjectProxy::preventExtensions(JSContext *cx, HandleObject proxy) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::getPropertyDescriptor(JSContext *cx, HandleObject wrapper, HandleId id,
                                       MutableHandle<PropertyDescriptor> desc) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::getOwnPropertyDescriptor(JSContext *cx, HandleObject wrapper, HandleId id,
                                          MutableHandle<PropertyDescriptor> desc) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::defineProperty(JSContext *cx, HandleObject wrapper, HandleId id,
                                MutableHandle<PropertyDescriptor> desc) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::getOwnPropertyNames(JSContext *cx, HandleObject wrapper,
                                     AutoIdVector &props) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::delete_(JSContext *cx, HandleObject wrapper, HandleId id, bool *bp) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::enumerate(JSContext *cx, HandleObject wrapper, AutoIdVector &props) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::call(JSContext *cx, HandleObject wrapper, const CallArgs &args) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::construct(JSContext *cx, HandleObject wrapper, const CallArgs &args) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::nativeCall(JSContext *cx, IsAcceptableThis test, NativeImpl impl,
                            CallArgs args) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::hasInstance(JSContext *cx, HandleObject proxy, MutableHandleValue v,
                             bool *bp) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::objectClassIs(HandleObject obj, ESClassValue classValue, JSContext *cx) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

const char *
DeadObjectProxy::className(JSContext *cx, HandleObject wrapper) const
{
    return "DeadObject";
}

JSString *
DeadObjectProxy::fun_toString(JSContext *cx, HandleObject proxy, unsigned indent) const
{
    return nullptr;
}

bool
DeadObjectProxy::regexp_toShared(JSContext *cx, HandleObject proxy, RegExpGuard *g) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::defaultValue(JSContext *cx, HandleObject obj, JSType hint,
                              MutableHandleValue vp) const
{
    JS_ReportErrorNumber(cx, js_GetErrorMessage, nullptr, JSMSG_DEAD_OBJECT);
    return false;
}

bool
DeadObjectProxy::getPrototypeOf(JSContext *cx, HandleObject proxy, MutableHandleObject protop) const
{
    protop.set(nullptr);
    return true;
}

const char DeadObjectProxy::family = 0;
const DeadObjectProxy DeadObjectProxy::singleton;

bool
js::IsDeadProxyObject(JSObject *obj)
{
    return obj->is<ProxyObject>() &&
           obj->as<ProxyObject>().handler() == &DeadObjectProxy::singleton;
}

void
js::NukeCrossCompartmentWrapper(JSContext *cx, JSObject *wrapper)
{
    JS_ASSERT(wrapper->is<CrossCompartmentWrapperObject>());

    NotifyGCNukeWrapper(wrapper);

    wrapper->as<ProxyObject>().nuke(&DeadObjectProxy::singleton);

    JS_ASSERT(IsDeadProxyObject(wrapper));
}

/*
 * NukeChromeCrossCompartmentWrappersForGlobal reaches into chrome and cuts
 * all of the cross-compartment wrappers that point to objects parented to
 * obj's global.  The snag here is that we need to avoid cutting wrappers that
 * point to the window object on page navigation (inner window destruction)
 * and only do that on tab close (outer window destruction).  Thus the
 * option of how to handle the global object.
 */
JS_FRIEND_API(bool)
js::NukeCrossCompartmentWrappers(JSContext* cx,
                                 const CompartmentFilter& sourceFilter,
                                 const CompartmentFilter& targetFilter,
                                 js::NukeReferencesToWindow nukeReferencesToWindow)
{
    CHECK_REQUEST(cx);
    JSRuntime *rt = cx->runtime();

    // Iterate through scopes looking for system cross compartment wrappers
    // that point to an object that shares a global with obj.

    for (CompartmentsIter c(rt, SkipAtoms); !c.done(); c.next()) {
        if (!sourceFilter.match(c))
            continue;

        // Iterate the wrappers looking for anything interesting.
        for (JSCompartment::WrapperEnum e(c); !e.empty(); e.popFront()) {
            // Some cross-compartment wrappers are for strings.  We're not
            // interested in those.
            const CrossCompartmentKey &k = e.front().key();
            if (k.kind != CrossCompartmentKey::ObjectWrapper)
                continue;

            AutoWrapperRooter wobj(cx, WrapperValue(e));
            JSObject *wrapped = UncheckedUnwrap(wobj);

            if (nukeReferencesToWindow == DontNukeWindowReferences &&
                wrapped->getClass()->ext.innerObject)
                continue;

            if (targetFilter.match(wrapped->compartment())) {
                // We found a wrapper to nuke.
                e.removeFront();
                NukeCrossCompartmentWrapper(cx, wobj);
            }
        }
    }

    return true;
}

// Given a cross-compartment wrapper |wobj|, update it to point to
// |newTarget|. This recomputes the wrapper with JS_WrapValue, and thus can be
// useful even if wrapper already points to newTarget.
bool
js::RemapWrapper(JSContext *cx, JSObject *wobjArg, JSObject *newTargetArg)
{
    RootedObject wobj(cx, wobjArg);
    RootedObject newTarget(cx, newTargetArg);
    JS_ASSERT(wobj->is<CrossCompartmentWrapperObject>());
    JS_ASSERT(!newTarget->is<CrossCompartmentWrapperObject>());
    JSObject *origTarget = Wrapper::wrappedObject(wobj);
    JS_ASSERT(origTarget);
    Value origv = ObjectValue(*origTarget);
    JSCompartment *wcompartment = wobj->compartment();

    AutoDisableProxyCheck adpc(cx->runtime());

    // If we're mapping to a different target (as opposed to just recomputing
    // for the same target), we must not have an existing wrapper for the new
    // target, otherwise this will break.
    JS_ASSERT_IF(origTarget != newTarget,
                 !wcompartment->lookupWrapper(ObjectValue(*newTarget)));

    // The old value should still be in the cross-compartment wrapper map, and
    // the lookup should return wobj.
    WrapperMap::Ptr p = wcompartment->lookupWrapper(origv);
    JS_ASSERT(&p->value().unsafeGet()->toObject() == wobj);
    wcompartment->removeWrapper(p);

    // When we remove origv from the wrapper map, its wrapper, wobj, must
    // immediately cease to be a cross-compartment wrapper. Neuter it.
    NukeCrossCompartmentWrapper(cx, wobj);

    // First, we wrap it in the new compartment. We try to use the existing
    // wrapper, |wobj|, since it's been nuked anyway. The wrap() function has
    // the choice to reuse |wobj| or not.
    RootedObject tobj(cx, newTarget);
    AutoCompartment ac(cx, wobj);
    if (!wcompartment->wrap(cx, &tobj, wobj))
        MOZ_CRASH();

    // If wrap() reused |wobj|, it will have overwritten it and returned with
    // |tobj == wobj|. Otherwise, |tobj| will point to a new wrapper and |wobj|
    // will still be nuked. In the latter case, we replace |wobj| with the
    // contents of the new wrapper in |tobj|.
    if (tobj != wobj) {
        // Now, because we need to maintain object identity, we do a brain
        // transplant on the old object so that it contains the contents of the
        // new one.
        if (!JSObject::swap(cx, wobj, tobj))
            MOZ_CRASH();
    }

    // Before swapping, this wrapper came out of wrap(), which enforces the
    // invariant that the wrapper in the map points directly to the key.
    JS_ASSERT(Wrapper::wrappedObject(wobj) == newTarget);

    // Update the entry in the compartment's wrapper map to point to the old
    // wrapper, which has now been updated (via reuse or swap).
    JS_ASSERT(wobj->is<WrapperObject>());
    wcompartment->putWrapper(cx, CrossCompartmentKey(newTarget), ObjectValue(*wobj));
    return true;
}

// Remap all cross-compartment wrappers pointing to |oldTarget| to point to
// |newTarget|. All wrappers are recomputed.
JS_FRIEND_API(bool)
js::RemapAllWrappersForObject(JSContext *cx, JSObject *oldTargetArg,
                              JSObject *newTargetArg)
{
    RootedValue origv(cx, ObjectValue(*oldTargetArg));
    RootedObject newTarget(cx, newTargetArg);

    AutoWrapperVector toTransplant(cx);
    if (!toTransplant.reserve(cx->runtime()->numCompartments))
        return false;

    for (CompartmentsIter c(cx->runtime(), SkipAtoms); !c.done(); c.next()) {
        if (WrapperMap::Ptr wp = c->lookupWrapper(origv)) {
            // We found a wrapper. Remember and root it.
            toTransplant.infallibleAppend(WrapperValue(wp));
        }
    }

    for (WrapperValue *begin = toTransplant.begin(), *end = toTransplant.end();
         begin != end; ++begin)
    {
        if (!RemapWrapper(cx, &begin->toObject(), newTarget))
            MOZ_CRASH();
    }

    return true;
}

JS_FRIEND_API(bool)
js::RecomputeWrappers(JSContext *cx, const CompartmentFilter &sourceFilter,
                      const CompartmentFilter &targetFilter)
{
    AutoWrapperVector toRecompute(cx);

    for (CompartmentsIter c(cx->runtime(), SkipAtoms); !c.done(); c.next()) {
        // Filter by source compartment.
        if (!sourceFilter.match(c))
            continue;

        // Iterate over the wrappers, filtering appropriately.
        for (JSCompartment::WrapperEnum e(c); !e.empty(); e.popFront()) {
            // Filter out non-objects.
            const CrossCompartmentKey &k = e.front().key();
            if (k.kind != CrossCompartmentKey::ObjectWrapper)
                continue;

            // Filter by target compartment.
            if (!targetFilter.match(static_cast<JSObject *>(k.wrapped)->compartment()))
                continue;

            // Add it to the list.
            if (!toRecompute.append(WrapperValue(e)))
                return false;
        }
    }

    // Recompute all the wrappers in the list.
    for (WrapperValue *begin = toRecompute.begin(), *end = toRecompute.end(); begin != end; ++begin)
    {
        JSObject *wrapper = &begin->toObject();
        JSObject *wrapped = Wrapper::wrappedObject(wrapper);
        if (!RemapWrapper(cx, wrapper, wrapped))
            MOZ_CRASH();
    }

    return true;
}
