#pragma once
#include <windows.h>
#include <cstdint>

// Minimal IL2CPP C API, resolved at runtime from GameAssembly.dll exports.
struct Il2CppDomain;
struct Il2CppAssembly;
struct Il2CppImage;
struct Il2CppClass;
struct Il2CppType;
struct Il2CppObject;
struct Il2CppString;
struct Il2CppThread;
struct FieldInfo;
struct MethodInfo;

#define IL2CPP_API_LIST(X)                                                                         \
    X(Il2CppDomain*, il2cpp_domain_get, ())                                                        \
    X(const Il2CppImage*, il2cpp_get_corlib, ())                                                   \
    X(const Il2CppAssembly**, il2cpp_domain_get_assemblies, (const Il2CppDomain*, size_t*))        \
    X(const Il2CppImage*, il2cpp_assembly_get_image, (const Il2CppAssembly*))                      \
    X(const char*, il2cpp_image_get_name, (const Il2CppImage*))                                    \
    X(size_t, il2cpp_image_get_class_count, (const Il2CppImage*))                                  \
    X(Il2CppClass*, il2cpp_image_get_class, (const Il2CppImage*, size_t))                          \
    X(Il2CppClass*, il2cpp_class_from_name, (const Il2CppImage*, const char*, const char*))        \
    X(const char*, il2cpp_class_get_name, (Il2CppClass*))                                          \
    X(const char*, il2cpp_class_get_namespace, (Il2CppClass*))                                     \
    X(Il2CppClass*, il2cpp_class_get_parent, (Il2CppClass*))                                       \
    X(Il2CppClass*, il2cpp_class_get_declaring_type, (Il2CppClass*))                               \
    X(Il2CppClass*, il2cpp_class_get_nested_types, (Il2CppClass*, void**))                         \
    X(const MethodInfo*, il2cpp_class_get_methods, (Il2CppClass*, void**))                         \
    X(FieldInfo*, il2cpp_class_get_fields, (Il2CppClass*, void**))                                 \
    X(const MethodInfo*, il2cpp_class_get_method_from_name, (Il2CppClass*, const char*, int))      \
    X(FieldInfo*, il2cpp_class_get_field_from_name, (Il2CppClass*, const char*))                   \
    X(const Il2CppType*, il2cpp_class_get_type, (Il2CppClass*))                                    \
    X(const char*, il2cpp_method_get_name, (const MethodInfo*))                                    \
    X(uint32_t, il2cpp_method_get_param_count, (const MethodInfo*))                                \
    X(const Il2CppType*, il2cpp_method_get_param, (const MethodInfo*, uint32_t))                   \
    X(const char*, il2cpp_method_get_param_name, (const MethodInfo*, uint32_t))                    \
    X(const Il2CppType*, il2cpp_method_get_return_type, (const MethodInfo*))                       \
    X(uint32_t, il2cpp_method_get_flags, (const MethodInfo*, uint32_t*))                           \
    X(const char*, il2cpp_field_get_name, (FieldInfo*))                                            \
    X(const Il2CppType*, il2cpp_field_get_type, (FieldInfo*))                                      \
    X(size_t, il2cpp_field_get_offset, (FieldInfo*))                                               \
    X(int, il2cpp_field_get_flags, (FieldInfo*))                                                   \
    X(void, il2cpp_field_static_get_value, (FieldInfo*, void*))                                    \
    X(void, il2cpp_field_get_value, (Il2CppObject*, FieldInfo*, void*))                            \
    X(char*, il2cpp_type_get_name, (const Il2CppType*))                                            \
    X(Il2CppClass*, il2cpp_object_get_class, (Il2CppObject*))                                      \
    X(Il2CppObject*, il2cpp_runtime_invoke, (const MethodInfo*, void*, void**, Il2CppObject**))    \
    X(Il2CppString*, il2cpp_string_new, (const char*))                                             \
    X(Il2CppThread*, il2cpp_thread_attach, (Il2CppDomain*))                                        \
    X(void*, il2cpp_object_unbox, (Il2CppObject*))                                                 \
    X(Il2CppObject*, il2cpp_type_get_object, (const Il2CppType*))                                  \
    X(uint32_t, il2cpp_array_length, (void*))                                                      \
    X(void, il2cpp_field_set_value, (Il2CppObject*, FieldInfo*, void*))                            \
    X(Il2CppObject*, il2cpp_array_new, (Il2CppClass*, uintptr_t))                                  \
    X(Il2CppClass*, il2cpp_class_get_element_class, (Il2CppClass*))                                \
    X(void, il2cpp_gc_wbarrier_set_field, (Il2CppObject*, void**, void*))                          \
    X(void, il2cpp_format_exception, (Il2CppObject*, char*, int))                                  \
    X(void, il2cpp_format_stack_trace, (Il2CppObject*, char*, int))                                \
    X(void, il2cpp_free, (void*))

#define IL2CPP_DECLARE(ret, name, args) \
    using name##_t = ret(*) args;       \
    inline name##_t name = nullptr;
IL2CPP_API_LIST(IL2CPP_DECLARE)
#undef IL2CPP_DECLARE

inline HMODULE g_gameAssembly = nullptr;

inline bool il2cpp_resolve_api()
{
    g_gameAssembly = GetModuleHandleW(L"GameAssembly.dll");
    if (!g_gameAssembly)
        return false;
    bool ok = true;
#define IL2CPP_RESOLVE(ret, name, args)                                         \
    name = reinterpret_cast<name##_t>(GetProcAddress(g_gameAssembly, #name));  \
    if (!name) ok = false;
    IL2CPP_API_LIST(IL2CPP_RESOLVE)
#undef IL2CPP_RESOLVE
    return ok;
}

// METHOD_ATTRIBUTE_STATIC / FIELD_ATTRIBUTE_STATIC
constexpr uint32_t kAttrStatic = 0x0010;

// First field of MethodInfo is the native method pointer (IL2CPP v24..v31).
inline void* il2cpp_method_pointer(const MethodInfo* m)
{
    return m ? *reinterpret_cast<void* const*>(m) : nullptr;
}
