#include <sprout/runtime/guest_runtime.h>

namespace sprout {

uint32_t GuestRuntime::alloc_fake_handle() {
    std::lock_guard<std::mutex> lock(handles_lock);
    if (next_fake_handle + 4 > kFakeHandleEnd) return kFakeHandleBase; /* exhausted: alias, harmless for opaque handles */
    uint32_t h = next_fake_handle;
    next_fake_handle += 4;
    return h;
}

uint32_t GuestRuntime::find_or_alloc_class(const std::string &name) {
    std::lock_guard<std::mutex> lock(jni_meta_lock);
    auto it = class_handle_by_name.find(name);
    if (it != class_handle_by_name.end()) return it->second;
    uint32_t h = alloc_fake_handle();
    class_handle_by_name[name] = h;
    class_name_by_handle[h] = name;
    return h;
}

uint32_t GuestRuntime::find_or_alloc_method(uint32_t class_handle, const std::string &name) {
    std::lock_guard<std::mutex> lock(jni_meta_lock);
    auto key = std::make_pair(class_handle, name);
    auto it = method_handle_by_key.find(key);
    if (it != method_handle_by_key.end()) return it->second;
    uint32_t h = alloc_fake_handle();
    method_handle_by_key[key] = h;
    method_key_by_handle[h] = key;
    return h;
}

void GuestRuntime::tag_object_class(uint32_t obj_addr, const std::string &class_name) {
    std::lock_guard<std::mutex> lock(jni_meta_lock);
    object_class_name[obj_addr] = class_name;
}

std::string GuestRuntime::class_name_of_object(uint32_t obj_addr) {
    std::lock_guard<std::mutex> lock(jni_meta_lock);
    auto it = object_class_name.find(obj_addr);
    return it != object_class_name.end() ? it->second : std::string();
}

/* The reverse of find_or_alloc_class: needed by NewObject, which is handed a
 * jclass and has to tag the instance it returns with that class's name. */
std::string GuestRuntime::class_name_of_handle(uint32_t class_handle) {
    std::lock_guard<std::mutex> lock(jni_meta_lock);
    auto it = class_name_by_handle.find(class_handle);
    return it != class_name_by_handle.end() ? it->second : std::string();
}

/* Allocates a JNI instance: a distinct non-null handle, tagged with its class
 * so GetObjectClass and the (class, method) hook lookup both work on it. */
uint32_t GuestRuntime::new_object(uint32_t class_handle) {
    std::lock_guard<std::mutex> lock(jni_meta_lock);
    uint32_t obj = alloc_fake_handle();
    auto it = class_name_by_handle.find(class_handle);
    if (it != class_name_by_handle.end()) object_class_name[obj] = it->second;
    return obj;
}

bool GuestRuntime::lookup_method(uint32_t method_id, std::string &out_class_name, std::string &out_method_name) {
    std::lock_guard<std::mutex> lock(jni_meta_lock);
    auto it = method_key_by_handle.find(method_id);
    if (it == method_key_by_handle.end()) return false;
    auto cn = class_name_by_handle.find(it->second.first);
    out_class_name = (cn != class_name_by_handle.end()) ? cn->second : std::string();
    out_method_name = it->second.second;
    return true;
}

GuestMutex *GuestRuntime::get_or_create_mutex(uint32_t addr) {
    std::lock_guard<std::mutex> lock(mutexes_lock);
    auto &slot = guest_mutexes[addr];
    if (!slot) slot = std::make_unique<GuestMutex>();
    return slot.get();
}

GuestCond *GuestRuntime::get_or_create_cond(uint32_t addr) {
    std::lock_guard<std::mutex> lock(conds_lock);
    auto &slot = guest_conds[addr];
    if (!slot) slot = std::make_unique<GuestCond>();
    return slot.get();
}

GuestSem *GuestRuntime::get_or_create_sem(uint32_t addr) {
    std::lock_guard<std::mutex> lock(sems_lock);
    auto &slot = guest_sems[addr];
    if (!slot) slot = std::make_unique<GuestSem>();
    return slot.get();
}

}  // namespace sprout
