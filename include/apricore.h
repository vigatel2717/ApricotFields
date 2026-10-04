
#ifndef APRICORE_H
#define APRICORE_H

#if __cplusplus
extern "C" {
#endif

/*
 * Every Apricot module's calls that can fail return an APRESULT: APRESULT_OK,
 * or a code from the module's own block (APRESULT_<MODULE>_*).
 */
typedef enum APRESULT {
	APRESULT_OK = 0,

	/* General: any module may return these. */

	/* A null pointer where one is required, an out-of-range value, a
	 * buffer of the wrong size, a call in the wrong state. */
	APRESULT_INVALID_ARGUMENT = 1,
	APRESULT_OUT_OF_MEMORY = 2,
	/* A caller-supplied I/O callback, or the I/O beneath it, failed. */
	APRESULT_IO = 3,
	APRESULT_NOT_FOUND = 4,
	APRESULT_NOT_IMPLEMENTED = 5,
	/* Over a limit the caller set. */
	APRESULT_LIMIT = 6,
	/* Valid input using a feature this module doesn't handle. */
	APRESULT_UNSUPPORTED = 7,
	/* Data that fails its integrity check: a checksum mismatch, or a
	 * decoded size other than the declared one. */
	APRESULT_CORRUPT_DATA = 8,

	/* ApArchive (archive/aparchive.h) */

	/* No end of central directory record: not a ZIP archive. */
	APRESULT_APARCHIVE_NOT_AN_ARCHIVE = 1001,
	/* A ZIP archive with a broken structure, or one that fails a check:
	 * overlapping entries, a local header that disagrees with the central
	 * directory, duplicate or unsafe names, a strict-mode violation. */
	APRESULT_APARCHIVE_MALFORMED = 1002,
	/* An entry compressed with a method that isn't in the codec table. */
	APRESULT_APARCHIVE_UNKNOWN_METHOD = 1003,
} APRESULT;

/*
 * In Debug configuration, every implementation of an Apricot opaque handle must
 * have its first member be a pointer to a null-terminated string containing the
 * debug name of the object, for use in debugging and validation layers. This is
 * to garauntee compatibility with casting, for a C++ std::string_view or
 * similar, so C++ backends can provide zero-cost debug names without extra
 * allocations.
 */
#ifdef _DEBUG
void apri_debug_name_set(void *object, const char *name);
#define APRI_SET_DEBUG_NAME(obj, name)                                         \
	apri_debug_name_set((void *)(obj), (name))
const char *apri_debug_name_get(void *object);
#define APRI_GET_DEBUG_NAME(obj) apri_debug_name_get((void *)(obj))
#endif

#if __cplusplus
}
#endif

#endif // APRICORE_H
