
#include "aprirenderbase.h"
#include "apricot_renderer_internal.hpp"

#ifdef _DEBUG
void apri_debug_name_set(void *object, const char *name)
{
    if (object)
    {
        /* Store the debug name in the first member of the object struct. */
        char **name_ptr = (char **)object;
        *name_ptr = (char *)name;
    }
}

const char *apri_debug_name_get(void *object)
{
    if (object)
    {
        /* Get the debug name from the first member of the object struct. */
        char **name_ptr = (char **)object;
        return *name_ptr;
    }
    else
        return nullptr;
}
#endif // _DEBUG

extern "C"
{
    aprend_instance aprend_instance_create(const aprend_instance_desc *desc)
    {
        if (!desc)
            return nullptr;
        ApricotRender::AprendInstance *result = new ApricotRender::AprendInstance();
        return reinterpret_cast<aprend_instance>(result);
    }
    aprend_instance_desc aprend_instance_get_desc(aprend_instance instance)
    {
        if (!instance)
            return {0};
        return reinterpret_cast<ApricotRender::AprendInstance *>(instance)->desc;
    }

    void aprend_instance_terminate(aprend_instance instance)
    {
        if (!instance)
            return;
        delete reinterpret_cast<ApricotRender::AprendInstance *>(instance);
    }
}
