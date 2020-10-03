#include "Enclave.h"
#include <queue>
#include <vector>

int num = 0;
int exit_count = 0;

hashtable* ht_enclave = NULL;
MACbuffer* MACbuf_enclave = NULL;
BucketMAC* MACTable = NULL;
Arg arg_enclave;
int ratio_root_per_buckets = 0;

sgx_thread_mutex_t global_mutex;
sgx_thread_mutex_t* queue_mutex;
sgx_thread_cond_t* job_cond;
std::vector<std::queue<job *> > queue;

/**
 * init enclave values 
 **/
void enclave_init_values(hashtable* ht_, MACbuffer* MACbuf_, Arg arg) {

	ht_enclave = ht_;	
	MACbuf_enclave = MACbuf_;

	arg_enclave = arg;

	//set the ratio of subtree root node inside the enclave memory 
	//over total hash value of buckets
	ratio_root_per_buckets = ht_enclave->size/arg_enclave.tree_root_size;

	MACTable = (BucketMAC*)malloc(sizeof(BucketMAC)* arg_enclave.tree_root_size);
	for(int i = 0; i < arg_enclave.tree_root_size; i++)
	{
		memset(MACTable[i].mac ,0 ,MAC_SIZE);
	}

	//Initialize mutex variables
	sgx_thread_mutex_init(&global_mutex, NULL);

	queue_mutex = (sgx_thread_mutex_t*)malloc(sizeof(sgx_thread_mutex_t)*arg_enclave.num_threads);
	job_cond = (sgx_thread_cond_t*)malloc(sizeof(sgx_thread_cond_t)*arg_enclave.num_threads);
	for(int i = 0 ; i < arg_enclave.num_threads; i++) {
		queue.push_back(std::queue<job*>());	
	}
	
}

/** 
 * processing set operation 
 **/
void enclave_set(char *cipher) { 

	char* key;
	char* val;
	char* key_val;
	char* plain_key_val = NULL;
	uint8_t nac[NAC_SIZE];
	uint8_t mac[MAC_SIZE];
	uint8_t prev_mac[MAC_SIZE];
	uint8_t updated_nac[NAC_SIZE];

	uint32_t key_size;
	uint32_t val_size;

	uint8_t key_idx;

	char *tok;
	char *temp_;
	entry * ret_entry;

	bool is_insert = false;

	tok = strtok_r(cipher+4," ",&temp_);
	key_size = strlen(tok)+1;
	key = (char*)malloc(sizeof(char)*key_size);
	memset(key, 0, key_size);
	memcpy(key, tok, key_size-1);

	val_size = strlen(temp_)+1;
	val = (char*)malloc(sizeof(char)*val_size);
	memset(val, 0, val_size);
	memcpy(val, temp_, val_size-1);

	int kv_pos = -1;

	ret_entry = ht_get_o(key, key_size, &plain_key_val, &kv_pos, updated_nac);
	
	int hash_val = ht_hash(key);
	key_idx = key_hash_func(key);

	sgx_status_t ret = SGX_SUCCESS;

	/* verifying integrity of data */
	//update
	if(ret_entry != NULL)
	{
		ret = enclave_verification(ret_entry->key_val, ret_entry->key_hash, ret_entry->key_size, ret_entry->val_size, ret_entry->nac, ret_entry->mac);

		if(ret != SGX_SUCCESS)
		{
			print("MAC verification failed");
		}

		memcpy(nac, updated_nac, NAC_SIZE);
		free(plain_key_val);

		is_insert = false;
		memcpy(prev_mac, ret_entry->mac, MAC_SIZE);
	}
	//insert
	else
	{
		/* Make initial nac */
		sgx_read_rand(nac, NAC_SIZE);
		assert(plain_key_val == NULL);
		is_insert = true;
	}


	/* We have to encrypt key and value together, so make con field */
	key_val = (char*)malloc(sizeof(char)*(key_size+val_size));
	memcpy(key_val, key, key_size);
	memcpy(key_val + key_size, val, val_size);

	enclave_encrypt(key_val, key_val, key_idx, key_size, val_size, nac, mac);

	ht_set_o(ret_entry, key, key_val, nac, mac, key_size, val_size, kv_pos);

	ret = enclave_rebuild_tree_root(hash_val, kv_pos, is_insert, prev_mac);

	if(ret != SGX_SUCCESS)
	{
		print("Tree verification failed");
	}

	memset(cipher, 0, arg_enclave.max_buf_size);
	memcpy(cipher, key, key_size);

	free(key);
	free(val);
	free(key_val);
}

/** 
 * processing get operation 
 **/
void enclave_get(char *cipher){ 

	char* key;
	char* plain_key_val = NULL;
	uint32_t key_size;

	char *tok;
	char *temp_;
	entry * ret_entry;

	uint8_t updated_nac[NAC_SIZE];

	tok = strtok_r(cipher+4," ",&temp_);
	key_size = strlen(tok)+1;
	key = (char*)malloc(sizeof(char)*key_size);
	memset(key, 0, key_size);
	memcpy(key, tok, key_size-1);

	int kv_pos = -1;
	ret_entry = ht_get_o(key, key_size, &plain_key_val, &kv_pos, updated_nac);

	if(ret_entry == NULL){
		print("GET FAILED: No data in database");
		return;
	}

	int hash_val = ht_hash(key);  
	sgx_status_t ret = SGX_SUCCESS;

	/* verifying integrity of data */
	ret = enclave_verification(ret_entry->key_val, ret_entry->key_hash, ret_entry->key_size, ret_entry->val_size, ret_entry->nac, ret_entry->mac);

	if(ret != SGX_SUCCESS)
	{
		print("MAC verification failed");
	}

	ret = enclave_verify_tree_root(hash_val);

	if(ret != SGX_SUCCESS)
	{
		print("Tree verification failed");
	}

	memset(cipher, 0, arg_enclave.max_buf_size);
	memcpy(cipher, plain_key_val+ret_entry->key_size, ret_entry->val_size);

	free(key);
	free(plain_key_val);

}

/**
 * processing append operation 
 **/
void enclave_append(char *cipher){ 

	char* key;
	char* val;
	char* key_val;
	char* plain_key_val;
	uint8_t nac[NAC_SIZE];
	uint8_t mac[MAC_SIZE];
	uint8_t prev_mac[MAC_SIZE];
	uint8_t updated_nac[NAC_SIZE];

	uint32_t key_size;
	uint32_t val_size;

	uint8_t key_idx;

	char *tok;
	char *temp_;
	entry * ret_entry;

	tok = strtok_r(cipher+4," ",&temp_);
	key_size = strlen(tok)+1;
	key = (char*)malloc(sizeof(char)*key_size);
	memset(key, 0, key_size);
	memcpy(key, tok, key_size-1);

	val_size = strlen(temp_)+1;
	val = (char*)malloc(sizeof(char)*val_size);
	memset(val, 0, val_size);
	memcpy(val, temp_, val_size-1);

	int kv_pos = -1;

	ret_entry = ht_get_o(key, key_size, &plain_key_val, &kv_pos, updated_nac);

	int hash_val = ht_hash(key);
	key_idx = key_hash_func(key);

	sgx_status_t ret = SGX_SUCCESS;

	/* verifying integrity of data */
	//update
	if(ret_entry != NULL)
	{
	
		ret = enclave_verification(ret_entry->key_val, ret_entry->key_hash, ret_entry->key_size, ret_entry->val_size, ret_entry->nac, ret_entry->mac);

		if(ret != SGX_SUCCESS)
		{
			print("MAC verification failed");
		}

		memcpy(nac, updated_nac, NAC_SIZE);
		memcpy(prev_mac, ret_entry->mac, MAC_SIZE);
	}
	//insert
	else
	{
		print("There's no data in the database");
		return;
	}


	/* Make appended key-value */
	key_val = (char*)malloc(sizeof(char)*(ret_entry->key_size + ret_entry->val_size + val_size - 1));
	memcpy(key_val, plain_key_val, ret_entry->key_size + ret_entry->val_size);
	memcpy(key_val + ret_entry->key_size + ret_entry->val_size - 1, val, val_size);

	enclave_encrypt(key_val, key_val, key_idx, key_size, ret_entry->val_size + val_size - 1, nac, mac);

	ht_append_o(ret_entry, key, key_val, nac, mac, key_size, ret_entry->val_size + val_size - 1, kv_pos);

	ret = enclave_rebuild_tree_root(hash_val, kv_pos, false, prev_mac);

	if(ret != SGX_SUCCESS)
	{
		print("Tree verification failed");
	}


	memset(cipher, 0, arg_enclave.max_buf_size);
	memcpy(cipher, key, key_size);

	free(key);
	free(val);
	free(plain_key_val);
	free(key_val);
}

/**
 * processing server working threads
 **/
void enclave_worker_thread(hashtable *ht_, MACbuffer *MACbuf_) {

	int thread_id;
	job *cur_job = NULL;
	char *cipher = NULL;

	ht_enclave = ht_;
	MACbuf_enclave = MACbuf_;

	sgx_thread_mutex_lock(&global_mutex);

	thread_id = num;
	num+=1;

	sgx_thread_mutex_init(&queue_mutex[thread_id], NULL);
	sgx_thread_cond_init(&job_cond[thread_id], NULL);

	sgx_thread_mutex_unlock(&global_mutex);

	sgx_thread_mutex_lock(&queue_mutex[thread_id]);
	
	while(1) {

		if(queue[thread_id].size() == 0) {
			sgx_thread_cond_wait(&job_cond[thread_id], &queue_mutex[thread_id]);
			continue;
		}

		cur_job = queue[thread_id].front();
		cipher = cur_job->buf;

		sgx_thread_mutex_unlock(&queue_mutex[thread_id]);


		if(strncmp(cipher, "GET", 3) == 0 || strncmp(cipher, "get", 3) == 0) {	
			enclave_get(cipher);
		}
		else if(strncmp(cipher, "SET", 3) == 0 || strncmp(cipher, "set", 3) == 0) {
			enclave_set(cipher);
		}
		else if(strncmp(cipher, "APP", 3) == 0 || strncmp(cipher, "app", 3) == 0) {
			enclave_append(cipher);
		}
		else if(strncmp(cipher, "quit", 4) == 0 ) {
			sgx_thread_mutex_lock(&queue_mutex[thread_id]);
			queue[thread_id].pop();
			free(cipher);
			free(cur_job);
			sgx_thread_mutex_unlock(&queue_mutex[thread_id]);

			sgx_thread_mutex_destroy(&queue_mutex[thread_id]);
			sgx_thread_cond_destroy(&job_cond[thread_id]);
			return;
		}
		else{
			print("Untyped request");
			break;
		}

		message_return(cipher, arg_enclave.max_buf_size, cur_job->client_sock);

		sgx_thread_mutex_lock(&queue_mutex[thread_id]);
		queue[thread_id].pop();
		free(cipher);
		free(cur_job);
	}

	return;

}

/**
 * Bring the resquest to enclave
 * parsing the key and send the requests to specific thread
 **/
void enclave_message_pass(void* data) {

	EcallParams *ecallParams = (EcallParams *)data;

	char* cipher = ecallParams->buf;
	int client_sock = ecallParams->client_sock_;
	int num_clients = ecallParams->num_clients_;

	char* key;
	uint32_t key_size;
	char *tok;
	char *temp_;
	int thread_id = 0;

	job* new_job = NULL;

	if(strncmp(cipher, "EXIT", 4) == 0 || strncmp(cipher, "exit", 4) == 0 ){
		message_return(cipher, arg_enclave.max_buf_size, client_sock);
		exit_count++;

		if(exit_count == num_clients) {
			print("All threads are finished");
			//Send exit message to all of the worker threads
			for(int i = 0 ; i < arg_enclave.num_threads; i++) {
				new_job = (job*)malloc(sizeof(job));
				new_job->buf = (char*)malloc(sizeof(char)*arg_enclave.max_buf_size);
				memset(new_job->buf, 0, arg_enclave.max_buf_size);
				memcpy(new_job->buf, "quit" , 4);
				new_job->client_sock = -1;

				sgx_thread_mutex_lock(&queue_mutex[i]);
				queue[i].push(new_job);
				sgx_thread_cond_signal(&job_cond[i]);
				sgx_thread_mutex_unlock(&queue_mutex[i]);
			}
		}
	}
	else if(strncmp(cipher, "LOADDONE", 8) == 0)
	{
		print("Load process is done");
		message_return(cipher, arg_enclave.max_buf_size, client_sock);
	}
	else {

		new_job = (job*)malloc(sizeof(job));
		new_job->buf = (char*)malloc(sizeof(char)*arg_enclave.max_buf_size);
		memcpy(new_job->buf, cipher, arg_enclave.max_buf_size);
		new_job->client_sock = client_sock;

		//parsing key
		tok = strtok_r(cipher+4," ",&temp_);
		key_size = strlen(tok)+1;
		key = (char*)malloc(sizeof(char)*key_size);
		memset(key, 0, key_size);
		memcpy(key, tok, key_size-1);

		//send the requests to specific worker thread
		thread_id = (int)(ht_hash(key)/(ht_enclave->size/arg_enclave.num_threads));
		free(key);
		sgx_thread_mutex_lock(&queue_mutex[thread_id]);
		queue[thread_id].push(new_job);
		sgx_thread_cond_signal(&job_cond[thread_id]);
		sgx_thread_mutex_unlock(&queue_mutex[thread_id]);
	}

	return;
}

/**
 * Hotcalls responder
 **/
void EcallStartResponder( HotCall* hotEcall )
{
	void (*callbacks[1])(void*);
	callbacks[0] = enclave_message_pass;

	HotCallTable callTable;
	callTable.numEntries = 1;
	callTable.callbacks  = callbacks;

	HotCall_waitForCall( hotEcall, &callTable );
}
