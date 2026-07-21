
#include "apricore.h"

#if __cplusplus
extern "C" {
#endif

#ifdef _DEBUG
void apri_debug_name_set(void *object, const char *name) {
	if (object) {
		/* Store the debug name in the first member of the object struct. */
		char **name_ptr = (char **)object;
		*name_ptr       = (char *)name;
	}
}

const char *apri_debug_name_get(void *object) {
	if (object) {
		/* Get the debug name from the first member of the object struct. */
		char **name_ptr = (char **)object;
		return *name_ptr;
	} else
		return nullptr;
}
#endif // _DEBUG

#if __cplusplus
}
#endif
