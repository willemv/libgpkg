/*
 * Copyright 2013 Luciad (http://www.luciad.com)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stdio.h>
#include "atomic_ops.h"
#include "geos_context.h"
#include "geos_geom_io.h"
#include "geom_func.h"
#include "spatialdb_internal.h"
#include "sql.h"

typedef struct {
  volatile long ref_count;
  GEOSContextHandle_t geos_handle;
  const spatialdb_t *spatialdb;
} geos_context_t;

static geos_context_t *geos_context_init(const spatialdb_t *spatialdb) {
  geos_context_t *ctx = sqlite3_malloc(sizeof(geos_context_t));
  if (ctx == NULL) {
    return NULL;
  }

  GEOSContextHandle_t geos_handle = geom_geos_init();
  if (geos_handle == NULL) {
    sqlite3_free(ctx);
    return NULL;
  }

  ctx->ref_count = 1;
  ctx->geos_handle = geos_handle;
  ctx->spatialdb = spatialdb;
  return ctx;
}

static void geos_context_acquire(geos_context_t *ctx) {
  if (ctx) {
    atomic_inc_long(&ctx->ref_count);
  }
}

static void geos_context_release(geos_context_t *ctx) {
  if (ctx) {
    long newval = atomic_dec_long(&ctx->ref_count);
    if (newval == 0) {
      geom_geos_destroy(ctx->geos_handle);
      ctx->geos_handle = NULL;
      sqlite3_free(ctx);
    }
  }
}

typedef struct {
  GEOSGeometry* geometry;
  GEOSContextHandle_t context;
  int srid;
} geos_geometry_t;

static geos_geometry_t *get_geos_geom(sqlite3_context *context, const geos_context_t *geos_context, sqlite3_value *value, errorstream_t *error) {
  geom_blob_header_t header;

  uint8_t *blob = (uint8_t *)sqlite3_value_blob(value);
  size_t blob_length = (size_t) sqlite3_value_bytes(value);

  if (blob == NULL) {
    return NULL;
  }

  binstream_t stream;
  binstream_init(&stream, blob, blob_length);

  geos_writer_t writer;
  geos_writer_init_srid(&writer, geos_context->geos_handle, header.srid);

  geos_context->spatialdb->read_blob_header(&stream, &header, error);
  geos_context->spatialdb->read_geometry(&stream, geos_writer_geom_consumer(&writer), error);

  GEOSGeometry *g = geos_writer_getgeometry(&writer);
  geos_writer_destroy(&writer, g == NULL);

  if (g == NULL) {
    return NULL;
  }

  geos_geometry_t *result = sqlite3_malloc(sizeof(geos_geometry_t));
  if (result == NULL) {
    return NULL;
  }

  result->context = geos_context->geos_handle;
  result->geometry = g;
  result->srid = header.srid;

  return result;
}

static void free_geos_geom(void* data) {
  if (data == NULL) {
    return;
  }

  geos_geometry_t* geom = (geos_geometry_t*)data;
  GEOSGeom_destroy_r(geom->context, geom->geometry);

  geom->context = NULL;
  geom->geometry = NULL;

  sqlite3_free(data);
}

typedef struct {
  const GEOSPreparedGeometry* geometry;
  GEOSContextHandle_t context;
  int srid;
} geos_prepared_geometry_t;

static geos_prepared_geometry_t *get_geos_prepared_geom(sqlite3_context *context, const geos_context_t *geos_context, sqlite3_value *value, errorstream_t *error) {
  geom_blob_header_t header;

  uint8_t *blob = (uint8_t *)sqlite3_value_blob(value);
  size_t blob_length = (size_t) sqlite3_value_bytes(value);

  if (blob == NULL) {
    return NULL;
  }

  binstream_t stream;
  binstream_init(&stream, blob, blob_length);

  geos_writer_t writer;
  geos_writer_init_srid(&writer, geos_context->geos_handle, header.srid);

  geos_context->spatialdb->read_blob_header(&stream, &header, error);
  geos_context->spatialdb->read_geometry(&stream, geos_writer_geom_consumer(&writer), error);

  GEOSGeometry *g = geos_writer_getgeometry(&writer);
  geos_writer_destroy(&writer, g == NULL);

  if (g == NULL) {
    return NULL;
  }

  struct GEOSPrepGeom_t const *prepared_g = GEOSPrepare_r(geos_context->geos_handle, g);
  if (prepared_g == NULL) {
    return NULL;
  }

  geos_prepared_geometry_t *result = sqlite3_malloc(sizeof(geos_prepared_geometry_t));
  if (result == NULL) {
    GEOSPreparedGeom_destroy_r(geos_context->geos_handle, prepared_g);
    return NULL;
  }

  result->context = geos_context->geos_handle;
  result->geometry = prepared_g;
  result->srid = header.srid;

  return result;
}

static void free_geos_prepared_geom(void* data) {
  if (data == NULL) {
    return;
  }

  geos_prepared_geometry_t* geom = (geos_prepared_geometry_t*)data;
  GEOSPreparedGeom_destroy_r(geom->context, geom->geometry);

  geom->context = NULL;
  geom->geometry = NULL;

  sqlite3_free(data);
}

static int set_geos_geom_result(sqlite3_context *context, const geos_context_t *geos_context, GEOSGeometry *geom, errorstream_t *error) {
  int result = SQLITE_OK;

  if (geom == NULL) {
    sqlite3_result_null(context);
    return result;
  } else {
    geom_blob_writer_t writer;
    geos_context->spatialdb->writer_init_srid(&writer, GEOSGetSRID_r(geos_context->geos_handle, geom));

    result = geos_read_geometry(geos_context->geos_handle, geom, geom_blob_writer_geom_consumer(&writer), error);

    if (result == SQLITE_OK) {
      sqlite3_result_blob(context, geom_blob_writer_getdata(&writer), geom_blob_writer_length(&writer), sqlite3_free);
    } else {
      sqlite3_result_error(context, error_message(error), -1);
    }

    geos_context->spatialdb->writer_destroy(&writer, 0);

    return result;
  }
}

#define GEOS_START(context) \
  const geos_context_t *geos_context = (const geos_context_t *)sqlite3_user_data(context); \
  char error_buffer[256];\
  errorstream_t error;\
  error_init_fixed(&error, error_buffer, 256)
#define GEOS_CONTEXT geos_context
#define GEOS_HANDLE geos_context->geos_handle

#define GEOS_GET_GEOM(name, args, i) \
  const geos_geometry_t *name = sqlite3_get_auxdata(context, i); \
  int name##_set_auxdata = 0; \
  if (name == NULL) { \
    name = get_geos_geom( context, geos_context, args[i], &error ); \
    name##_set_auxdata = 1;\
  }
#define GEOS_FREE_GEOM(name, i) \
  if (name != NULL && name##_set_auxdata) { \
    sqlite3_set_auxdata(context, i, (void*)name, free_geos_geom); \
  }

#define GEOS_GET_PREPARED_GEOM(name, args, i) \
  const geos_prepared_geometry_t *name = sqlite3_get_auxdata(context, i); \
  int name##_set_auxdata = 0; \
  if (name == NULL) { \
    name = get_geos_prepared_geom( context, geos_context, args[i], &error ); \
    name##_set_auxdata = 1;\
  }
#define GEOS_FREE_PREPARED_GEOM(name, i) \
  if (name != NULL && name##_set_auxdata) { \
    sqlite3_set_auxdata(context, i, (void*)name, free_geos_prepared_geom); \
  }

#define GEOS_FUNC_GEOM__INTEGER_(sql_name, geos_name) static void ST_##sql_name(sqlite3_context *context, int nbArgs, sqlite3_value **args) {\
  GEOS_START(context);\
  GEOS_GET_GEOM( g1, args, 0 );\
  if (g1 == NULL) {\
    if (error_count(&error) > 0) {\
      sqlite3_result_error(context, error_message(&error), -1);\
    } else {\
      sqlite3_result_null(context);\
    }\
    return;\
  }\
  char result = GEOS##geos_name##_r(GEOS_HANDLE, g1->geometry );\
  if (result == 2) {\
    geom_geos_get_error(&error);\
    sqlite3_result_error(context, error_message(&error), -1);\
  } else {\
    sqlite3_result_int(context, result);\
  }\
  GEOS_FREE_GEOM( g1, 0 );\
}

#define GEOS_FUNC_GEOM_INTEGER__SUBGEOM_(sql_name, geos_name) static void ST_##sql_name(sqlite3_context *context, int nbArgs, sqlite3_value **args) {\
  GEOS_START(context);\
  GEOS_GET_GEOM( g1, args, 0 );\
  int n = sqlite3_value_int( args[1] );\
  if (g1 == NULL) {\
    if (error_count(&error) > 0) {\
      sqlite3_result_error(context, error_message(&error), -1);\
    } else {\
      sqlite3_result_null(context);\
    }\
    return;\
  }\
  GEOSGeometry *result = GEOS##geos_name##_r(GEOS_HANDLE, g1->geometry, n);\
  if (result != NULL) {\
    set_geos_geom_result(context, GEOS_CONTEXT, result, &error);\
  } else {\
    geom_geos_get_error(&error);\
    sqlite3_result_error(context, error_message(&error), -1);\
  }\
  GEOS_FREE_GEOM( g1, 0 );\
}

#define GEOS_FUNC_GEOM_INTEGER__GEOM(name) GEOS_FUNC_GEOM__INTEGER_(name, name)

#define GEOS_FUNC_GEOM_GEOM__INTEGER(name) static void ST_##name(sqlite3_context *context, int nbArgs, sqlite3_value **args) {\
  GEOS_START(context);\
  GEOS_GET_GEOM( g1, args, 0 );\
  GEOS_GET_GEOM( g2, args, 1 );\
  if (g1 == NULL || g2 == NULL) {\
    if (error_count(&error) > 0) {\
      sqlite3_result_error(context, error_message(&error), -1);\
    } else {\
      sqlite3_result_null(context);\
    }\
    return;\
  }\
  int srid1 = g1->srid;\
  int srid2 = g2->srid;\
  if (srid1 != srid2 ) {\
    error_append(&error, "Cannot apply %s when SRIDs differ: %d != %d", #name, srid1, srid2);\
    sqlite3_result_error(context, error_message(&error), -1);\
    return;\
  }\
  char result = GEOS##name##_r(GEOS_HANDLE, g1->geometry, g2->geometry);\
  if (result == 2) {\
    geom_geos_get_error(&error);\
    sqlite3_result_error(context, error_message(&error), -1);\
  } else {\
    sqlite3_result_int(context, result);\
  }\
  GEOS_FREE_GEOM( g1, 0 );\
  GEOS_FREE_GEOM( g2, 1 );\
}

#define GEOS_FUNC_PREPGEOM_GEOM__INTEGER(name) static void ST_##name(sqlite3_context *context, int nbArgs, sqlite3_value **args) {\
  GEOS_START(context);\
  GEOS_GET_PREPARED_GEOM( g1, args, 0 );\
  GEOS_GET_GEOM( g2, args, 1 );\
  if (g1 == NULL || g2 == NULL) {\
    if (error_count(&error) > 0) {\
      sqlite3_result_error(context, error_message(&error), -1);\
    } else {\
      sqlite3_result_null(context);\
    }\
    return;\
  }\
  int srid1 = g1->srid;\
  int srid2 = g2->srid;\
  if (srid1 != srid2 ) {\
    error_append(&error, "Cannot apply %s when SRIDs differ: %d != %d", #name, srid1, srid2);\
    sqlite3_result_error(context, error_message(&error), -1);\
    return;\
  }\
  char result = GEOSPrepared##name##_r(GEOS_HANDLE, g1->geometry, g2->geometry);\
  if (result == 2) {\
    geom_geos_get_error(&error);\
    sqlite3_result_error(context, error_message(&error), -1);\
  } else {\
    sqlite3_result_int(context, result);\
  }\
  GEOS_FREE_PREPARED_GEOM( g1, 0 );\
  GEOS_FREE_GEOM( g2, 0 );\
}

#define GEOS_FUNC_GEOM__DOUBLE(name) static void ST_##name(sqlite3_context *context, int nbArgs, sqlite3_value **args) {\
  GEOS_START(context);\
  GEOS_GET_GEOM( g1, args, 0 );\
  if (g1 == NULL) {\
    if (error_count(&error) > 0) {\
      sqlite3_result_error(context, error_message(&error), -1);\
    } else {\
      sqlite3_result_null(context);\
    }\
    return;\
  }\
  double val;\
  char result = GEOS##name##_r(GEOS_HANDLE, g1->geometry, &val);\
  if (result == 1) {\
    sqlite3_result_double(context, val);\
  } else {\
    geom_geos_get_error(&error);\
    sqlite3_result_error(context, error_message(&error), -1);\
  }\
  GEOS_FREE_GEOM( g1, 0 );\
}

#define GEOS_FUNC_GEOM_GEOM__DOUBLE(name) static void ST_##name(sqlite3_context *context, int nbArgs, sqlite3_value **args) {\
  GEOS_START(context);\
  GEOS_GET_GEOM( g1, args, 0 );\
  GEOS_GET_GEOM( g2, args, 1 );\
  if (g1 == NULL || g2 == NULL) {\
    if (error_count(&error) > 0) {\
      sqlite3_result_error(context, error_message(&error), -1);\
    } else {\
      sqlite3_result_null(context);\
    }\
    return;\
  }\
  int srid1 = g1->srid;\
  int srid2 = g2->srid;\
  if (srid1 != srid2 ) {\
    error_append(&error, "Cannot apply %s when SRIDs differ: %d != %d", #name, srid1, srid2);\
    sqlite3_result_error(context, error_message(&error), -1);\
    return;\
  }\
  double val;\
  char result = GEOS##name##_r(GEOS_HANDLE, g1->geometry, g2->geometry, &val);\
  if (result == 1) {\
    sqlite3_result_double(context, val);\
  } else {\
    geom_geos_get_error(&error);\
    sqlite3_result_error(context, error_message(&error), -1);\
  }\
  GEOS_FREE_GEOM( g1, 0 );\
  GEOS_FREE_GEOM( g2, 1 );\
}

#define GEOS_FUNC_GEOM__GEOM_(sql_name, geos_name) static void ST_##sql_name(sqlite3_context *context, int nbArgs, sqlite3_value **args) {\
  GEOS_START(context);\
  GEOS_GET_GEOM( g1, args, 0 );\
  if (g1 == NULL) {\
    if (error_count(&error) > 0) {\
      sqlite3_result_error(context, error_message(&error), -1);\
    } else {\
      sqlite3_result_null(context);\
    }\
    return;\
  }\
  GEOSGeometry *result = GEOS##geos_name##_r(GEOS_HANDLE, g1->geometry);\
  if (result != NULL) {\
    set_geos_geom_result(context, GEOS_CONTEXT, result, &error);\
    GEOSGeom_destroy_r( GEOS_HANDLE, result );\
  } else {\
    geom_geos_get_error(&error);\
    sqlite3_result_error(context, error_message(&error), -1);\
  }\
  GEOS_FREE_GEOM( g1, 0 );\
}

#define GEOS_FUNC_GEOM__GEOM(name) GEOS_FUNC_GEOM__GEOM_(name, name)

#define GEOS_FUNC_GEOM__SUBGEOM_(sql_name, geos_name) static void ST_##sql_name(sqlite3_context *context, int nbArgs, sqlite3_value **args) {\
  GEOS_START(context);\
  GEOS_GET_GEOM( g1, args, 0 );\
  if (g1 == NULL) {\
    if (error_count(&error) > 0) {\
      sqlite3_result_error(context, error_message(&error), -1);\
    } else {\
      sqlite3_result_null(context);\
    }\
    return;\
  }\
  GEOSGeometry *result = GEOS##geos_name##_r(GEOS_HANDLE, g1->geometry);\
  if (result != NULL) {\
    set_geos_geom_result(context, GEOS_CONTEXT, result, &error);\
  } else {\
    geom_geos_get_error(&error);\
    sqlite3_result_error(context, error_message(&error), -1);\
  }\
  GEOS_FREE_GEOM( g1, 0 );\
}

#define GEOS_FUNC_GEOM_GEOM__GEOM(name) static void ST_##name(sqlite3_context *context, int nbArgs, sqlite3_value **args) {\
  GEOS_START(context);\
  GEOS_GET_GEOM( g1, args, 0 );\
  GEOS_GET_GEOM( g2, args, 1 );\
  if (g1 == NULL || g2 == NULL) {\
    if (error_count(&error) > 0) {\
      sqlite3_result_error(context, error_message(&error), -1);\
    } else {\
      sqlite3_result_null(context);\
    }\
    return;\
  }\
  int srid1 = g1->srid;\
  int srid2 = g2->srid;\
  if (srid1 != srid2 ) {\
    error_append(&error, "Cannot apply %s when SRIDs differ: %d != %d", #name, srid1, srid2);\
    sqlite3_result_error(context, error_message(&error), -1);\
    return;\
  }\
  GEOSGeometry *result = GEOS##name##_r(GEOS_HANDLE, g1->geometry, g2->geometry);\
  if (result != NULL) {\
    set_geos_geom_result(context, GEOS_CONTEXT, result, &error);\
    GEOSGeom_destroy_r( GEOS_HANDLE, result );\
  } else {\
    geom_geos_get_error(&error);\
    sqlite3_result_error(context, error_message(&error), -1);\
  }\
  GEOS_FREE_GEOM( g1, 0 );\
  GEOS_FREE_GEOM( g2, 1 );\
}

static void ST_Relate(sqlite3_context *context, int nbArgs, sqlite3_value **args) {
  GEOS_START(context);
  GEOS_GET_GEOM(g1, args, 0);
  GEOS_GET_GEOM(g2, args, 1);
  const unsigned char *pattern = sqlite3_value_text(args[2]);
  if (g1 == NULL || g2 == NULL || pattern == NULL) {
    if (error_count(&error) > 0) {
      sqlite3_result_error(context, error_message(&error), -1);
    } else {
      sqlite3_result_null(context);
    }
    return;
  }

  char result = GEOSRelatePattern_r(GEOS_HANDLE, g1->geometry, g2->geometry, (const char *)pattern);
  if (result == 2) {
    geom_geos_get_error(&error);
    sqlite3_result_error(context, error_message(&error), -1);
  } else {
    sqlite3_result_int(context, result);
  }
  GEOS_FREE_GEOM(g1, 0);
  GEOS_FREE_GEOM(g2, 1);
}

// Copied from geos::operation::buffer::BufferParameters
static const int DEFAULT_QUADRANT_SEGMENTS = 8;

static void ST_Buffer(sqlite3_context *context, int nbArgs, sqlite3_value **args) {
  GEOS_START(context);
  GEOS_GET_GEOM(g1, args, 0);
  double distance = sqlite3_value_double(args[1]);
  if (g1 == NULL) {
    if (error_count(&error) > 0) {
      sqlite3_result_error(context, error_message(&error), -1);
    } else {
      sqlite3_result_null(context);
    }
    return;
  }

  GEOSGeometry *result = GEOSBuffer_r(GEOS_HANDLE, g1->geometry, distance, DEFAULT_QUADRANT_SEGMENTS);
  if (result != NULL) {
    set_geos_geom_result(context, GEOS_CONTEXT, result, &error);
    GEOSGeom_destroy_r( GEOS_HANDLE, result );
  } else {
    geom_geos_get_error(&error);
    sqlite3_result_error(context, error_message(&error), -1);
  }
  GEOS_FREE_GEOM( g1, 0 );
}

GEOS_FUNC_GEOM__INTEGER_(IsSimple, isSimple)
GEOS_FUNC_GEOM__INTEGER_(IsRing, isRing)

GEOS_FUNC_GEOM__INTEGER_(IsValid, isValid)

GEOS_FUNC_PREPGEOM_GEOM__INTEGER(Disjoint)
GEOS_FUNC_PREPGEOM_GEOM__INTEGER(Intersects)
GEOS_FUNC_PREPGEOM_GEOM__INTEGER(Touches)
GEOS_FUNC_PREPGEOM_GEOM__INTEGER(Crosses)
GEOS_FUNC_PREPGEOM_GEOM__INTEGER(Within)
GEOS_FUNC_PREPGEOM_GEOM__INTEGER(Contains)
GEOS_FUNC_PREPGEOM_GEOM__INTEGER(Overlaps)

GEOS_FUNC_GEOM_GEOM__INTEGER(Equals)

GEOS_FUNC_GEOM__DOUBLE(Area)
GEOS_FUNC_GEOM__DOUBLE(Length)

GEOS_FUNC_GEOM_GEOM__DOUBLE(Distance)
GEOS_FUNC_GEOM_GEOM__DOUBLE(HausdorffDistance)

GEOS_FUNC_GEOM__GEOM(Boundary)
GEOS_FUNC_GEOM__GEOM(ConvexHull)
GEOS_FUNC_GEOM__GEOM(Envelope)

GEOS_FUNC_GEOM__GEOM_(Centroid, GetCentroid)

GEOS_FUNC_GEOM__INTEGER_(NumPoints, GeomGetNumPoints)
GEOS_FUNC_GEOM_INTEGER__SUBGEOM_(PointN, GeomGetPointN)
GEOS_FUNC_GEOM__SUBGEOM_(StartPoint, GeomGetStartPoint)
GEOS_FUNC_GEOM__SUBGEOM_(EndPoint, GeomGetEndPoint)

GEOS_FUNC_GEOM__INTEGER_(NumInteriorRings, GetNumInteriorRings)
GEOS_FUNC_GEOM_INTEGER__SUBGEOM_(InteriorRingN, GetInteriorRingN)
GEOS_FUNC_GEOM__SUBGEOM_(ExteriorRing, GetExteriorRing)

GEOS_FUNC_GEOM__INTEGER_(NumGeometries, GetNumGeometries)
GEOS_FUNC_GEOM_INTEGER__SUBGEOM_(GeometryN, GetGeometryN)

GEOS_FUNC_GEOM_GEOM__GEOM(Difference)
GEOS_FUNC_GEOM_GEOM__GEOM(SymDifference)
GEOS_FUNC_GEOM_GEOM__GEOM(Intersection)
GEOS_FUNC_GEOM_GEOM__GEOM(Union)

#if GEOS_VERSION_MAJOR > 3 || (GEOS_VERSION_MAJOR == 3 && GEOS_VERSION_MINOR >= 3)
GEOS_FUNC_GEOM__INTEGER_(IsClosed, isClosed)
GEOS_FUNC_PREPGEOM_GEOM__INTEGER(Covers)
GEOS_FUNC_PREPGEOM_GEOM__INTEGER(CoveredBy)
#endif

static void GPKG_GEOSVersion(sqlite3_context *context, int nbArgs, sqlite3_value **args) {
  sqlite3_result_text(context, GEOSversion(), -1, SQLITE_TRANSIENT);
}

#define STR(x) #x

#define GEOS_FUNCTION(db, prefix, name, nbArgs, ctx, error)                                                            \
  do {                                                                                                                 \
    geos_context_acquire(ctx);                                                                                         \
    sql_create_function(db, STR(name), prefix##_##name, nbArgs, SQL_DETERMINISTIC, ctx, (void(*)(void*))geos_context_release, error);     \
    geos_context_acquire(ctx);                                                                                         \
    sql_create_function(db, STR(prefix##_##name), prefix##_##name, nbArgs, SQL_DETERMINISTIC, ctx, (void(*)(void*))geos_context_release, error);\
  } while (0)

void geom_func_init(sqlite3 *db, const spatialdb_t *spatialdb, errorstream_t *error) {
  geos_context_t *ctx = geos_context_init(spatialdb);
  if (ctx == NULL) {
    error_append(error, "Error allocating GEOS context");
    return;
  }

  int geos_major;
  int geos_minor;
  int geos_version_result = sscanf(GEOSversion(), "%d.%d", &geos_major, &geos_minor);
  if (geos_version_result != 2) {
    error_append(error, "Could not parse GEOS version number (%s)", GEOSversion());
  }

  GEOS_FUNCTION(db, ST, Area, 1, ctx, error);
  GEOS_FUNCTION(db, ST, Length, 1, ctx, error);

  GEOS_FUNCTION(db, ST, IsSimple, 1, ctx, error);
  GEOS_FUNCTION(db, ST, IsRing, 1, ctx, error);
  GEOS_FUNCTION(db, ST, IsValid, 1, ctx, error);

  GEOS_FUNCTION(db, ST, Disjoint, 2, ctx, error);
  GEOS_FUNCTION(db, ST, Intersects, 2, ctx, error);
  GEOS_FUNCTION(db, ST, Touches, 2, ctx, error);
  GEOS_FUNCTION(db, ST, Crosses, 2, ctx, error);
  GEOS_FUNCTION(db, ST, Within, 2, ctx, error);
  GEOS_FUNCTION(db, ST, Contains, 2, ctx, error);
  GEOS_FUNCTION(db, ST, Overlaps, 2, ctx, error);
  GEOS_FUNCTION(db, ST, Equals, 2, ctx, error);
  GEOS_FUNCTION(db, ST, Relate, 3, ctx, error);

  GEOS_FUNCTION(db, ST, Distance, 2, ctx, error);
  GEOS_FUNCTION(db, ST, HausdorffDistance, 2, ctx, error);

  GEOS_FUNCTION(db, ST, Boundary, 1, ctx, error);
  GEOS_FUNCTION(db, ST, ConvexHull, 1, ctx, error);
  GEOS_FUNCTION(db, ST, Envelope, 1, ctx, error);

  GEOS_FUNCTION(db, ST, Difference, 2, ctx, error);
  GEOS_FUNCTION(db, ST, SymDifference, 2, ctx, error);
  GEOS_FUNCTION(db, ST, Intersection, 2, ctx, error);
  GEOS_FUNCTION(db, ST, Union, 2, ctx, error);

  GEOS_FUNCTION(db, ST, Buffer, 2, ctx, error);

  GEOS_FUNCTION(db, ST, Centroid, 1, ctx, error);

  GEOS_FUNCTION(db, ST, NumPoints, 1, ctx, error);
  GEOS_FUNCTION(db, ST, PointN, 2, ctx, error);
  GEOS_FUNCTION(db, ST, StartPoint, 1, ctx, error);
  GEOS_FUNCTION(db, ST, EndPoint, 1, ctx, error);

  GEOS_FUNCTION(db, ST, NumInteriorRings, 1, ctx, error);
  GEOS_FUNCTION(db, ST, InteriorRingN, 2, ctx, error);
  GEOS_FUNCTION(db, ST, ExteriorRing, 1, ctx, error);

  GEOS_FUNCTION(db, ST, NumGeometries, 1, ctx, error);
  GEOS_FUNCTION(db, ST, GeometryN, 2, ctx, error);

#if GEOS_VERSION_MAJOR > 3 || (GEOS_VERSION_MAJOR == 3 && GEOS_VERSION_MINOR >= 3)
  if (geos_major > 3 || (geos_major == 3 && geos_minor >= 3)) {
    GEOS_FUNCTION(db, ST, IsClosed, 1, ctx, error);
    GEOS_FUNCTION(db, ST, Covers, 2, ctx, error);
    GEOS_FUNCTION(db, ST, CoveredBy, 2, ctx, error);
  }
#endif

  GEOS_FUNCTION(db, GPKG, GEOSVersion, 0, ctx, error);

  geos_context_release(ctx);
}
