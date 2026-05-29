#ifndef APRICOT_MATH_H
#define APRICOT_MATH_H

/* Shared math types for all Apricot modules.
 * Plain C structs — memory-layout compatible with GLM types in C++ backends.
 * Conversion in C++ is a zero-cost reinterpret_cast. */

typedef struct { float  x, y;          } ApriVec2;
typedef struct { float  x, y, z;       } ApriVec3;
typedef struct { float  x, y, z, w;    } ApriVec4;
typedef struct { double x, y, z;       } ApriDVec3;
typedef struct { double x, y, z, w;    } ApriDVec4;
typedef struct { float  x, y, z, w;    } ApriQuat;
typedef struct { float  m[16];         } ApriMat4;   /* column-major */
typedef struct { double m[16];         } ApriDMat4;  /* column-major */

#endif /* APRICOT_MATH_H */
