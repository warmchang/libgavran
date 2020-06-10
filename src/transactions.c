#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "transactions.h"
#include "errors.h"
#include "pal.h"


typedef struct page_hash_entry {
    uint64_t page_num;
    void* address;
} page_hash_entry_t;

struct transaction_state {
    db_state_t* db;
    size_t allocated_size;
    uint32_t flags;
    uint32_t modified_pages;
    page_hash_entry_t entries[];
};

struct database_state{
    database_options_t options;
    file_header_t header;
    void* address;
    uint64_t address_size;
    file_handle_t* handle;
};

#define get_number_of_buckets(state) ((state->allocated_size - sizeof(txn_state_t)) / sizeof(page_hash_entry_t))

bool commit_transaction(txn_t* tx){
    txn_state_t* state = tx->state;
    size_t number_of_buckets = get_number_of_buckets(state);
    
    for(size_t i = 0; i< number_of_buckets; i++){
        if(!state->entries[i].address)
            continue;

        if(!write_file(state->db->handle, 
            state->entries[i].page_num * PAGE_SIZE, 
            state->entries[i].address, PAGE_SIZE)) {
                push_error(EIO, "Unable to write page %lu", state->entries[i].page_num);
            return false;
        }
        free(state->entries[i].address);
        state->entries[i].address = 0;
    }
    return true;
}

 bool close_transaction(txn_t* tx) {
     if(!tx->state)
        return true; // probably double close?
    txn_state_t* state = tx->state;
    size_t number_of_buckets = get_number_of_buckets(state);
    bool result = true;
    for(size_t i = 0; i< number_of_buckets; i++){
        if(!state->entries[i].address)
            continue;

        free(state->entries[i].address);
        state->entries[i].address = 0;
    }

    free(tx->state);

    tx->state = 0;
    return result;
 }

bool create_transaction(database_t* db, uint32_t flags, txn_t* tx){
    
    assert_no_existing_errors();

    size_t initial_size = sizeof(txn_state_t) + sizeof(page_hash_entry_t) * 8;
    txn_state_t* state = calloc(1, initial_size);
    if (!state){
       push_error(ENOMEM, "Unable to allocate memory for transaction state");
       return false;
    }
    memset(state, 0, initial_size);
    state->allocated_size = initial_size;
    state->flags = flags;
    state->db = db->state;

    tx->state = state;
    return true;
}


static bool lookup_entry_in_tx(txn_state_t* state, uint64_t page_num, page_hash_entry_t** entry){
    size_t number_of_buckets = get_number_of_buckets(state);
    size_t starting_pos = (size_t)(page_num % number_of_buckets);
    // we use linear probing to find a value in case of collisions
    for(size_t i = 0; i < number_of_buckets; i++){
        size_t index = (i + starting_pos) % number_of_buckets;
        if(!state->entries[index].address){
            // empty value, so there is no match
            return false;
        }
        if(state->entries[index].page_num == page_num){
            *entry = &state->entries[index];
            return true;
        }
    }
    return false;
}

enum hash_resize_status {
    hash_resize_success,
    hash_resize_err_no_mem,
    hash_resize_err_failure,
};

static enum hash_resize_status expand_hash_table(txn_state_t** state_ptr, size_t number_of_buckets){
    size_t new_number_of_buckets = number_of_buckets*2;
    size_t new_size = sizeof(txn_state_t) + (new_number_of_buckets*sizeof(page_hash_entry_t));
    txn_state_t* state  = *state_ptr;
    txn_state_t* new_state = calloc(1, new_size);
    if (!new_state){
        // we are OOM, but we'll accept that and let the hash
        // table fill to higher capacity, caller may decide to 
        // error
        return hash_resize_err_no_mem;
    }
    memcpy(new_state, state, sizeof(txn_state_t));
    new_state->allocated_size = new_size;

    for(size_t i = 0; i < number_of_buckets; i++){
        if(!state->entries[i].address)
            continue;
        size_t starting_pos = state->entries[i].page_num % new_number_of_buckets;
        bool located = false;
        for(size_t j = 0; j < new_number_of_buckets; j++){
            size_t index = (j + starting_pos) % new_number_of_buckets;
            if(!new_state->entries[index].address){ // empty
                new_state->entries[index] = state->entries[i];
                located = true;
                break;
            }
        }
        if(!located){
            push_error(EINVAL, "Failed to find spot for %lu after hash table resize", state->entries[i].page_num);
            free(new_state);
            return hash_resize_err_failure;
        }
    }

    *state_ptr = new_state;// update caller's reference
    free(state);
    return hash_resize_success;
}

static bool allocate_entry_in_tx(txn_state_t** state_ptr, uint64_t page_num, page_hash_entry_t** entry){
    txn_state_t* state = *state_ptr;
    size_t number_of_buckets = get_number_of_buckets(state);
    size_t starting_pos = (size_t)(page_num % number_of_buckets);
    // we use linear probing to find a value in case of collisions
    for(size_t i = 0; i < number_of_buckets; i++){
        size_t index = (i + starting_pos) % number_of_buckets;
        if(state->entries[index].page_num == page_num && state->entries[index].address ){
            push_error(EINVAL, "Attempted to allocate entry for page %lu which already exist in the table", page_num);
            return false;
        }

        if(!state->entries[index].address){
            size_t max_pages = (number_of_buckets * 3/4);
            // check the load factor
            if(state->modified_pages+1 < max_pages){
                state->modified_pages++;
                state->entries[index].page_num = page_num;
                *entry = &state->entries[index];
                return true;    
            }
            switch(expand_hash_table(state_ptr, number_of_buckets)){
                case hash_resize_success:
                    // try again, now we'll have enough room
                    return allocate_entry_in_tx(state_ptr, page_num, entry);
                case hash_resize_err_no_mem:
                    // we'll accept it here and just have higher
                    // load factor
                    break; 
                case hash_resize_err_failure:
                    push_error(EINVAL, "Failed to add page %lu to the transaction hash table", page_num);
                    return false;
            }
        }
    }

     switch(expand_hash_table(state_ptr, number_of_buckets)){
        case hash_resize_success:
            // try again, now we'll have enough room
            return allocate_entry_in_tx(state_ptr, page_num, entry);
        case hash_resize_err_no_mem: 
            // we are at 100% capacity, can't recover, will error now
            push_error(ENOMEM, "Can't allocate to add page %lu to the transaction hash table", page_num);
            return false;
        case hash_resize_err_failure:
            push_error(EINVAL, "Failed to add page %lu to the transaction hash table", page_num);
            return false;
    }
}

bool get_page(txn_t* tx, page_t* page){
    assert_no_existing_errors();

    page_hash_entry_t* entry;
    if(lookup_entry_in_tx(tx->state, page->page_num, &entry)) {
        page->address = entry->address;
        return true;
    }
    uint64_t offset = page->page_num * PAGE_SIZE;
    if(offset + PAGE_SIZE > tx->state->db->address_size){
        push_error(ERANGE, "Requests page %lu is outside of the bounds of the file (%lu)", 
            page->page_num, tx->state->db->address_size);
        return false;
    }

    page->address = ((char*)tx->state->db->address + offset);
    return true;
}

bool modify_page(txn_t* tx, page_t* page) {
    assert_no_existing_errors();

    page_hash_entry_t* entry;
    if(lookup_entry_in_tx(tx->state, page->page_num, &entry)) {
        page->address = entry->address;
        return true;
    }

    uint64_t offset = page->page_num * PAGE_SIZE;
    if(offset + PAGE_SIZE > tx->state->db->address_size){
        push_error(ERANGE, "Requests page %lu is outside of the bounds of the file (%lu)", 
            page->page_num, tx->state->db->address_size);
        return false;
    }
    void* original = ((char*)tx->state->db->address + offset);
    void* modified;
    int rc = posix_memalign(&modified, PAGE_ALIGNMENT, PAGE_SIZE);
    if (rc){
        push_error(rc, "Unable to allocate memory for a COW page %lu", page->page_num);
        return false;
    }
    memcpy(modified, original, PAGE_SIZE);
    if(!allocate_entry_in_tx(&tx->state, page->page_num, &entry)){
        mark_error();
        free(modified);
        return false;
    }
    entry->address = modified;
    page->address = modified;
    return true;
}

static void close_transaction_p(void *p){
    if(!close_transaction(p)){
        mark_error();
    }
}

static MUST_CHECK bool set_file_headers(database_t * db){
    txn_t tx;
    if(!create_transaction(db, 0, &tx)){
        push_error(EINVAL, "Failed to create a transaction on database init: %s", get_file_name(db->state->handle));
        return false;
    }
    defer(close_transaction_p, &tx);
    page_t page = {0, 0};
    if(!modify_page(&tx, &page)){
        push_error(EINVAL, "Unable to get first page of file: %s", get_file_name(db->state->handle));
        return false;
    }
    memcpy(page.address, &db->state->header, sizeof(file_header_t));
    memcpy((char*)page.address + PAGE_SIZE/2, &db->state->header, sizeof(file_header_t));

    if(!commit_transaction(&tx)){
        push_error(EINVAL, "Unable to commit init transaction on: %s", get_file_name(db->state->handle));
        return false;
    }

    return true;
}

static MUST_CHECK bool handle_newly_opened_database(database_t* db){
    db_state_t* state = db->state;
    file_header_t* header1 = state->address;
    file_header_t* header2 = (void*)((char*)state->address + PAGE_SIZE/2);
    
    // at this point state->header is zeroed
    // if both headers are zeroed, we are probably dealing with a new database
    bool isNew = !memcmp(header1, &state->header, sizeof(file_header_t)) && 
                 !memcmp(header2, &state->header, sizeof(file_header_t));

    if(isNew) {
        // now needs to set it up
        state->header.magic = FILE_HEADER_MAGIC;
        state->header.version = 1;
        state->header.page_size = PAGE_SIZE;
        state->header.number_of_pages = state->address_size / PAGE_SIZE;
        if(!set_file_headers(db)){
            mark_error();
            return false;
        }
    }
    file_header_t* selected = header1->magic != FILE_HEADER_MAGIC ? header2: header1;
    if (selected->magic != FILE_HEADER_MAGIC) {
        push_error(EINVAL, "Unable to find matching header in first page of %s", get_file_name(state->handle));
        return false;
    }
    if(selected->number_of_pages * PAGE_SIZE > state->address_size){
        push_error(EINVAL, "The size of the file %s is %lu but expected to have %lu pages, file was probably truncated", 
            get_file_name(state->handle), state->address_size, selected->number_of_pages * PAGE_SIZE 
        );
        return false;
    }
    if(selected->page_size != PAGE_SIZE){
        push_error(EINVAL, "File %s page size is %d, expected %d", 
            get_file_name(state->handle), 
            selected->page_size,
            PAGE_SIZE );
        return false;
    }

    return true;
}

static MUST_CHECK bool close_database_state(db_state_t* state){
    if(!state)
        return true; // double close?
    bool result = true;
    if(state->address){
        if(unmap_file(state->address, state->address_size)){
            mark_error();
            result = false;
        }
    }
    if(!close_file(state->handle)){
        mark_error();
        result = false;
    }
    free(state);
    return result;
}

static void close_database_state_p(void* p){
    if(!close_database_state(p)){
        mark_error();
    }
}

bool close_database(database_t* db){
    if(!db->state)
        return true;// double close?
    bool result = close_database_state(db->state);
    db->state = 0;
    return result;
}

static MUST_CHECK bool validate_options(database_options_t* options){

    if(!options->minimum_size)
        options->minimum_size = 128*1024;

    if(options->minimum_size < 128*1024){
        push_error(EINVAL, "The minimum_size cannot be less than %d", 128*1024);
        return false;
    }
        
    if(options->minimum_size % PAGE_SIZE) { 
        push_error(EINVAL, "The minimum size must be page aligned (%d)", PAGE_SIZE);
        return false;
    }

    return true;
}

bool open_database(const char* path, database_options_t* options, database_t* db){

    db_state_t* ptr = 0;
    // defer is called on the _pointer_ of
    // ptr, not its value;
    size_t* cancel;
    try_defer(close_database_state_p, &ptr, cancel);

    if(!validate_options(options)){
        mark_error();
        return false;
    }

    size_t db_state_size;
    if(!get_file_handle_size(path, &db_state_size)){
        mark_error();
        return false;
    }
    db_state_size += sizeof(db_state_t);

    ptr = calloc(1, db_state_size);
    if(!ptr){
        push_error(ENOMEM, 
            "Unable to allocate "
            "database state struct: %s", path);
        return false;
    }
    ptr->handle = (file_handle_t*)(ptr+1);

    if(!create_file(path, ptr->handle)){
        push_error(EIO, "Unable to create file for %s", path);
        return false;// cleanup via defer
    }

    ptr->options = *options;
    if(!ensure_file_minimum_size(ptr->handle,   
            options->minimum_size)){
        mark_error();
        return false;// cleanup via defer
    }
    if(!get_file_size(ptr->handle, &ptr->address_size)){
        mark_error();
        return false;// cleanup via defer
    }

    if(!map_file(ptr->handle, 0, 
            ptr->address_size, &ptr->address)){
       mark_error();
       return false;// cleanup via defer
    }

    db->state = ptr;

    if(!handle_newly_opened_database(db)){
        mark_error();
        return false;// cleanup via defer
    }

    *cancel = 1;// ensuring that defer won't clean it
    return true;
}

