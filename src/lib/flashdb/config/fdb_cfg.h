#ifndef _FDB_CFG_H_
#define _FDB_CFG_H_

#define FDB_USING_KVDB

#ifdef FDB_USING_KVDB
/* #define FDB_KV_AUTO_UPDATE */
#endif

/* #define FDB_USING_TSDB */

#define FDB_USING_FAL_MODE

#ifdef FDB_USING_FAL_MODE
#define FDB_WRITE_GRAN  1
#endif

/* #define FDB_DEBUG_ENABLE */

#endif /* _FDB_CFG_H_ */
