/*
	Copyright (C) 2013 University of Lugano

	This file is part of LibPaxos.

	LibPaxos is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	Libpaxos is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with LibPaxos.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "storage.h"
#include "paxos_config.h"

#include <db.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <assert.h>

#define MEM_CACHE_SIZE (0), (32*1024*1024)

struct storage
{
	DB* db;
	DB_ENV* env;
	DB_TXN* txn;
	int acceptor_id;
	char record_buf[PAXOS_MAX_VALUE_SIZE];
};

static int 
bdb_init_tx_handle(struct storage* s, int tx_mode, char* db_env_path)
{
	int result;
	DB_ENV* dbenv; 
	
	//Create environment handle
	result = db_env_create(&dbenv, 0);
	if (result != 0) {
		printf("DB_ENV creation failed: %s\n", db_strerror(result));
		return -1;
	}
	
	//Durability mode, see paxos_config.h
	result = dbenv->set_flags(dbenv, tx_mode, 1);
	if (result != 0) {
		printf("DB_ENV set_flags failed: %s\n", db_strerror(result));
		return -1;
	}
	
	//Redirect errors to sdout
	dbenv->set_errfile(dbenv, stdout);
    
	//Set the size of the memory cache
	result = dbenv->set_cachesize(dbenv, MEM_CACHE_SIZE, 1);
	if (result != 0) {
		printf("DB_ENV set_cachesize failed: %s\n", db_strerror(result));
		return -1;
	}
	
	//TODO see page size impact
	//Set page size for this db
	// result = dbp->set_pagesize(dbp, pagesize);
	// assert(result  == 0);

	//FIXME set log size


	// Environment open flags
	int flags;
	flags =
		DB_CREATE       |  /* Create if not existing */ 
		DB_RECOVER      |  /* Run normal recovery. */
		DB_INIT_LOCK    |  /* Initialize the locking subsystem */
		DB_INIT_LOG     |  /* Initialize the logging subsystem */
		DB_INIT_TXN     |  /* Initialize the transactional subsystem. */
		//DB_PRIVATE      |  /* DB is for this process only */
		DB_THREAD       |  /* Cause the environment to be free-threaded */  
		DB_REGISTER 	|
		DB_INIT_MPOOL;     /* Initialize the memory pool (in-memory cache) */

	//Open the DB environment
	result = dbenv->open(dbenv, 
		db_env_path,            /* Environment directory */
		flags,                  /* Open flags */
		0);                     /* Default file permissions */

	if (result != 0) {
		printf("DB_ENV open failed: %s\n", db_strerror(result));
		return -1;
	}

	s->env = dbenv;
	return 0;
}

static int
bdb_init_db(struct storage* s, char* db_path)
{
	int result;
	DB* dbp;
	
	//Create the DB file
	result = db_create(&(s->db), s->env, 0);
	if (result != 0) {
		printf("db_create failed: %s\n", db_strerror(result));
		return -1;
	}
	
	dbp = s->db;
    
	if (DURABILITY_MODE == 0 || DURABILITY_MODE == 20) {
		//Set the size of the memory cache
		result = dbp->set_cachesize(dbp, MEM_CACHE_SIZE, 1);
		if (result != 0) {
			printf("DBP set_cachesize failed: %s\n", db_strerror(result));
			return -1;
		}
	}
    
	// DB flags
	int flags = DB_CREATE;          /*Create if not existing */

	storage_tx_begin(s);

	//Open the DB file
	result = dbp->open(dbp,
		s->txn,                    /* Transaction pointer */
		db_path,                /* On-disk file that holds the database. */
		NULL,                   /* Optional logical database name */
		ACCEPTOR_ACCESS_METHOD, /* Database access method */
		flags,                  /* Open flags */
		0);                     /* Default file permissions */

	storage_tx_commit(s);

	if (result != 0) {
		printf("DB open failed: %s\n", db_strerror(result));
		return -1;
	}
	return 0;
}

struct storage*
storage_open(int acceptor_id, int do_recovery)
{
	char db_env_path[512];
	char db_filename[512];
	char db_file_path[512];
	
	struct storage* s = malloc(sizeof(struct storage));
	memset(s, 0, sizeof(struct storage));
	
	s->acceptor_id = acceptor_id;

	//Create path to db file in db dir
	sprintf(db_env_path, ACCEPTOR_DB_PATH);    
	sprintf(db_filename, ACCEPTOR_DB_FNAME);
	sprintf(db_file_path, "%s/%s", db_env_path, db_filename);
	LOG(VRB, ("Opening db file %s/%s\n", db_env_path, db_filename));    

	struct stat sb;
	//Check if the environment dir and db file exists
	int dir_exists = (stat(db_env_path, &sb) == 0);
	int db_exists = (stat(db_file_path, &sb) == 0);

    //Create the directory if it does not exist
	if (!dir_exists && (mkdir(db_env_path, S_IRWXU) != 0)) {
		printf("Failed to create env dir %s: %s\n", 
			db_env_path, strerror(errno));
		return NULL;
	} 
    
	//Delete and recreate an empty dir if not recovering
	if (!do_recovery && dir_exists) {
		char rm_command[600];
		sprintf(rm_command, "rm -r %s", db_env_path);

		if ((system(rm_command) != 0) || 
			(mkdir(db_env_path, S_IRWXU) != 0)) {
			printf("Failed to recreate empty env dir %s: %s\n",
			db_env_path, strerror(errno));
		}
	}

	int ret = 0;
	char * db_file = db_filename;
	switch (DURABILITY_MODE) {
		//In memory cache
		case 0: {
			//Give full path if opening without handle
			db_file = db_file_path;
		}
		break;

        //Transactional storage
		case 10: {
			ret = bdb_init_tx_handle(s, DB_LOG_IN_MEMORY, db_env_path);
		}
		break;

		case 11: {
			ret = bdb_init_tx_handle(s, DB_TXN_NOSYNC, db_env_path);
		}
		break;

		case 12: {
			ret = bdb_init_tx_handle(s, DB_TXN_WRITE_NOSYNC, db_env_path);
		}
		break;

		case 13: {
			ret = bdb_init_tx_handle(s, 0, db_env_path);
		}
		break;

		case 20: {
			//Give full path if opening without handle
			db_file = db_file_path;
		}
		break;

		default: {
			printf("Unknow durability mode %d!\n", DURABILITY_MODE);
			return NULL;
		}
	}
    
	if (ret != 0) {
		printf("Failed to open DB handle\n");
	}
    
	if (bdb_init_db(s, db_file) != 0) {
		printf("Failed to open DB file\n");
		return NULL;
	}
	
	return s;
}

int
storage_close(struct storage* s)
{	
	int result = 0;
	DB* dbp = s->db;
	DB_ENV* dbenv = s->env;
	
	//Close db file
	if(dbp->close(dbp, 0) != 0) {
		printf("DB_ENV close failed\n");
		result = -1;
	}

	switch(DURABILITY_MODE) {
		case 0:
		case 20:
		break;
        
		//Transactional storage
		case 10:
		case 11:
		case 12:
		case 13: {
			//Close handle
			if(dbenv->close(dbenv, 0) != 0) {
				printf("DB close failed\n");
				result = -1;
			}
		}
		break;
        
		default: {
			printf("Unknow durability mode %d!\n", DURABILITY_MODE);
			return -1;
		}
	}    
 
	LOG(VRB, ("DB close completed\n"));  
	return result;
	
}

void
storage_tx_begin(struct storage* s)
{
	if (DURABILITY_MODE == 0 || DURABILITY_MODE == 20) {
		return;
	}

	int result;
	result = s->env->txn_begin(s->env, NULL, &s->txn, 0);
	assert(result == 0);	
}

void
storage_tx_commit(struct storage* s)
{
	int result;

	if (DURABILITY_MODE == 0) {
		return;
	}

	if (DURABILITY_MODE == 20) {
		result = s->db->sync(s->db, 0);
		assert(result == 0);
		return;
	}

	// Since it's either read only or write only
	// and there is no concurrency, should always commit!
	result = s->txn->commit(s->txn, 0);
	assert(result == 0);
}

acceptor_record* 
storage_get_record(struct storage* s, iid_t iid)
{
	int flags, result;
	DBT dbkey, dbdata;
	DB* dbp = s->db;
	DB_TXN* txn = s->txn;
	acceptor_record* record_buffer = (acceptor_record*)s->record_buf;


	memset(&dbkey, 0, sizeof(DBT));
	memset(&dbdata, 0, sizeof(DBT));

	//Key is iid
	dbkey.data = &iid;
	dbkey.size = sizeof(iid_t);
    
	//Data is our buffer
	dbdata.data = record_buffer;
	dbdata.ulen = PAXOS_MAX_VALUE_SIZE;
	//Force copy to the specified buffer
	dbdata.flags = DB_DBT_USERMEM;

	//Read the record
	flags = 0;
	result = dbp->get(dbp, 
		txn,
		&dbkey,
		&dbdata,
		flags);
    
	if (result == DB_NOTFOUND || result == DB_KEYEMPTY) {
		//Record does not exist
		LOG(DBG, ("The record for iid:%u does not exist\n", iid));
		return NULL;
	} else if (result != 0) {
		//Read error!
		printf("Error while reading record for iid:%u : %s\n",
		iid, db_strerror(result));
		return NULL;
	}
    
	//Record found
	assert(iid == record_buffer->iid);
	return record_buffer;
}

acceptor_record*
storage_save_accept(struct storage* s, accept_req * ar)
{
	int flags, result;
	DBT dbkey, dbdata;
	DB* dbp = s->db;
	DB_TXN* txn = s->txn;
	acceptor_record* record_buffer = (acceptor_record*)s->record_buf;

    
	//Store as acceptor_record (== accept_ack)
	record_buffer->acceptor_id = s->acceptor_id;
	record_buffer->iid = ar->iid;
	record_buffer->ballot = ar->ballot;
	record_buffer->value_ballot = ar->ballot;
	record_buffer->is_final = 0;
	record_buffer->value_size = ar->value_size;
	memcpy(record_buffer->value, ar->value, ar->value_size);
    
	memset(&dbkey, 0, sizeof(DBT));
	memset(&dbdata, 0, sizeof(DBT));

	//Key is iid
	dbkey.data = &ar->iid;
	dbkey.size = sizeof(iid_t);
        
	//Data is our buffer
	dbdata.data = record_buffer;
	dbdata.size = ACCEPT_ACK_SIZE(record_buffer);
    
	//Store permanently
	flags = 0;
	result = dbp->put(dbp, 
		txn, 
		&dbkey, 
		&dbdata, 
	0);

	assert(result == 0);    
	return record_buffer;
}

acceptor_record*
storage_save_prepare(struct storage* s, prepare_req* pr, acceptor_record* rec)
{
	int flags, result;
	DBT dbkey, dbdata;
	DB* dbp = s->db;
	DB_TXN* txn = s->txn;
	acceptor_record* record_buffer = (acceptor_record*)s->record_buf;

    
	//No previous record, create a new one
	if (rec == NULL) {
		//Record does not exist yet
		rec = record_buffer;
		rec->acceptor_id = s->acceptor_id;
		rec->iid = pr->iid;
		rec->ballot = pr->ballot;
		rec->value_ballot = 0;
		rec->is_final = 0;
		rec->value_size = 0;
	} else {
		//Record exists, just update the ballot
		rec->ballot = pr->ballot;
	}
    
	memset(&dbkey, 0, sizeof(DBT));
	memset(&dbdata, 0, sizeof(DBT));

	//Key is iid
	dbkey.data = &pr->iid;
	dbkey.size = sizeof(iid_t);
        
	//Data is our buffer
	dbdata.data = record_buffer;
	dbdata.size = ACCEPT_ACK_SIZE(record_buffer);
    
	//Store permanently
	flags = 0;
	result = dbp->put(dbp, 
		txn, 
		&dbkey, 
		&dbdata, 
		0);
        
	assert(result == 0);
	return record_buffer;	
}

acceptor_record*
storage_save_final_value(struct storage* s, char* value, size_t size, 
	iid_t iid, ballot_t b)
{
	int flags, result;
	DBT dbkey, dbdata;
	DB* dbp = s->db;
	DB_TXN* txn = s->txn;
	acceptor_record* record_buffer = (acceptor_record*)s->record_buf;
	
	//Store as acceptor_record (== accept_ack)
	record_buffer->iid = iid;
	record_buffer->ballot = b;
	record_buffer->value_ballot = b;
	record_buffer->is_final = 1;
	record_buffer->value_size = size;
	memcpy(record_buffer->value, value, size);
	
	memset(&dbkey, 0, sizeof(DBT));
	memset(&dbdata, 0, sizeof(DBT));
	
	//Key is iid
	dbkey.data = &iid;
	dbkey.size = sizeof(iid_t);
	
	//Data is our buffer
	dbdata.data = record_buffer;
	dbdata.size = ACCEPT_ACK_SIZE(record_buffer);
	
	//Store permanently
	flags = 0;
		result = dbp->put(dbp, 
		txn, 
		&dbkey, 
		&dbdata, 
		0);
	
	assert(result == 0);    
	return record_buffer;
}

/*@
 * Walk through the database and return the highest iid we have seen. 
 * If this is a RECNO database this can probably be done more cheaply
 * but for now we will make this usable regardless of the index impl
 */
iid_t 
storage_get_max_iid(struct storage * s)
{
	int ret;
	DB *dbp = s->db;
	DBC *dbcp;
	DBT key, data;
	iid_t max_iid = 0;
	
	/* Acquire a cursor for the database. */
	if ((ret = dbp->cursor(dbp, NULL, &dbcp, 0)) != 0) {
		dbp->err(dbp, ret, "DB->cursor");
		return (1);
	}
	
	/* Re-initialize the key/data pair. */ memset(&key, 0, sizeof(key));
	memset(&data, 0, sizeof(data));
	
	/* Walk through the database and print out the key/data pairs. */
	while ((ret = dbcp->c_get(dbcp, &key, &data, DB_NEXT)) == 0) {
		assert(data.size = sizeof(iid_t));
		max_iid = *(iid_t *)key.data;
	}
	
	if (ret != DB_NOTFOUND) {
		dbp->err(dbp, ret, "DBcursor->get");
		return 0;
	}
	
	/* Close the cursor. */
	if ((ret = dbcp->c_close(dbcp)) != 0) {
		dbp->err(dbp, ret, "DBcursor->close");
	}
	return (max_iid);
}
