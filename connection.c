/*-------------------------------------------------------------------------
 *
 * connection.h
 *		  Foreign-data wrapper for remote MySQL servers
 *
 * Portions Copyright (c) 2012-2014, PostgreSQL Global Development Group
 *
 * Portions Copyright (c) 2004-2014, EnterpriseDB Corporation.
 *
 * IDENTIFICATION
 *		  connection.h
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "mysql_fdw.h"

#include "access/xact.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

/* Length of host */
#define HOST_LEN 256

/*
 * Connection cache hash table entry
 *
 * The lookup key in this hash table is the foreign MySQL server name / IP
 * and the server port number. (We use just one connection per user per foreign server,
 * so that we can ensure all scans use the same snapshot during a query.)
 *
 * The "conn" pointer can be NULL if we don't currently have a live connection.
 */
typedef struct ConnCacheKey
{
	char host[HOST_LEN];	/* MySQL's host name  / IP address */
	int32 port;				/* MySQL's port number */
} ConnCacheKey;

typedef struct ConnCacheEntry
{
	ConnCacheKey key;		/* hash key (must be first) */
	MYSQL *conn;            /* connection to foreign server, or NULL */
} ConnCacheEntry;

/*
 * Connection cache (initialized on first use)
 */
static HTAB *ConnectionHash = NULL;

/*
 * mysql_get_connection:
 * 			Get a connection which can be used to execute queries on
 * the remote MySQL server with the user's authorization. A new connection
 * is established if we don't already have a suitable one.
 */
MYSQL*
mysql_get_connection(mysql_opt *opt)
{
    bool found;
	ConnCacheEntry *entry;
	ConnCacheKey key;
    
 	/* First time through, initialize connection cache hashtable */
	if (ConnectionHash == NULL)
	{
		HASHCTL	ctl;
		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(ConnCacheKey);
		ctl.entrysize = sizeof(ConnCacheEntry);
		ctl.hash = tag_hash;
        
		/* allocate ConnectionHash in the cache context */
		ctl.hcxt = CacheMemoryContext;
		ConnectionHash = hash_create("mysql_fdw connections", 8,
                                     &ctl,
                                     HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
	}
    
	/* Create hash key for the entry */
	memset(key.host, 0, HOST_LEN);
	strncpy(key.host, opt->svr_address, HOST_LEN);
	key.port = opt->svr_port;
    
	/*
	 * Find or create cached entry for requested connection.
	 */
	entry = hash_search(ConnectionHash, &key, HASH_ENTER, &found);
	if (!found)
	{
		/* initialize new hashtable entry (key is already filled in) */
		entry->conn = NULL;
	}
	if (entry->conn == NULL)
	{
		entry->conn = mysql_connect(opt->svr_address, opt->svr_username, opt->svr_password, opt->svr_database, opt->svr_port);
		elog(DEBUG3, "new mysql_fdw connection %p for server \"%s:%d\"",
			 entry->conn, opt->svr_address, opt->svr_port);
	}
	return entry->conn;
}

/*
 * cleanup_connection:
 * Delete all the cache entries on backend exists.
 */
void
mysql_cleanup_connection(void)
{
	HASH_SEQ_STATUS	scan;
	ConnCacheEntry *entry;
    
	if (ConnectionHash == NULL)
		return;
    
	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		if (entry->conn == NULL)
			continue;
        
		elog(DEBUG3, "disconnecting mysql_fdw connection %p", entry->conn);
		mysql_close(entry->conn);
		entry->conn = NULL;
	}
}

/*
 * Release connection created by calling GetConnection.
 */
void
mysql_rel_connection(MYSQL *conn)
{
	/*
	 * We don't close the connection indvisually  here, will do all connection
	 * cleanup on the backend exit.
	 */
}


MYSQL*
mysql_connect(char *svr_address, char *svr_username, char *svr_password, char *svr_database, int svr_port)
{
	MYSQL *conn = NULL;

	/* Connect to the server */
	conn = mysql_init(NULL);
	if (!conn)
		ereport(ERROR,
			(errcode(ERRCODE_FDW_OUT_OF_MEMORY),
			errmsg("failed to initialise the MySQL connection object")
			));

	mysql_options(conn, MYSQL_SET_CHARSET_NAME, GetDatabaseEncodingName());

	if (!mysql_real_connect(conn, svr_address, svr_username, svr_password, svr_database, svr_port, NULL, 0))
		ereport(ERROR,
			(errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
			errmsg("failed to connect to MySQL: %s", mysql_error(conn))
			));
	return conn;
}
