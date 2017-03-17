/*
	JUCI Backend Websocket API Server

	Copyright (C) 2016 Martin K. Schröder <mkschreder.uk@gmail.com>

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version. (Please read LICENSE file on special
	permission to include this software in signed images). 

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.
*/

#include <dirent.h>
#include <pthread.h>

#include "internal.h"
#include "orange_luaobject.h"
#include "orange_lua.h"

#define JUCI_LUA_LIB_PATH "/usr/lib/orange/lib/"

static lua_State * _luaobject_create_lua_state(void){
	lua_State *L = luaL_newstate(); 		
	// export server api to the lua object
	orange_lua_publish_json_api(L); 
	orange_lua_publish_file_api(L); 
	orange_lua_publish_session_api(L); 
	orange_lua_publish_core_api(L); 
	return L; 
}

struct orange_luaobject* orange_luaobject_new(const char *name){
	struct orange_luaobject *self = calloc(1, sizeof(struct orange_luaobject)); 
	assert(self); 
	self->lua = _luaobject_create_lua_state(); 
	self->name = malloc(strlen(name) + 1); 
	strcpy(self->name, name); 
	self->avl.key = self->name; 
	luaL_openlibs(self->lua); 
	blob_init(&self->signature, 0, 0); 

	// add proper lua paths
	lua_getglobal(self->lua, "package"); 
	lua_getfield(self->lua, -1, "path"); 
	char newpath[255];
	const char *dirs[] = {
		getenv("JUCI_LUA_LIB_PATH"),
		"./lualib/",
		JUCI_LUA_LIB_PATH,
	}; 
	const char *lua_libs = 0; 
	for(int c = 0; c < 3; c++){
		lua_libs = dirs[c]; 
		if(!lua_libs) continue; 
		DIR *dir = opendir(lua_libs); 
		if(dir) { closedir(dir); break; }
	}
	if(!lua_libs) lua_libs = "./"; 
	snprintf(newpath, 255, "%s/?.lua;%s/orange/?.lua;%s;?.lua", lua_libs, lua_libs, lua_tostring(self->lua, -1)); 
	//TRACE("LUA: using lua path: %s\n", newpath); 
	lua_pop(self->lua, 1); 
	lua_pushstring(self->lua, newpath); 
	lua_setfield(self->lua, -2, "path"); 
	lua_pop(self->lua, 1); 

	pthread_mutex_init(&self->lock, NULL); 

	return self; 
}

void orange_luaobject_free_state(struct orange_luaobject *self){
	if(!self->lua) return; 
	lua_close(self->lua); 
	self->lua = 0; 
}

void orange_luaobject_delete(struct orange_luaobject **self){
	orange_luaobject_free_state(*self); 
	blob_free(&(*self)->signature); 
	free((*self)->name); 
	pthread_mutex_destroy(&(*self)->lock); 
	free(*self); 
	*self = NULL; 
}

int orange_luaobject_load(struct orange_luaobject *self, const char *file){
	pthread_mutex_lock(&self->lock); 

	if(!self->lua) self->lua = _luaobject_create_lua_state();  
	if(luaL_loadfile(self->lua, file) != 0){
		ERROR("could not load plugin: %s\n", lua_tostring(self->lua, -1)); 
		pthread_mutex_unlock(&self->lock); 
		return -1; 
	}
	if(lua_pcall(self->lua, 0, 1, 0) != 0){
		ERROR("could not run plugin: %s\n", lua_tostring(self->lua, -1)); 
		pthread_mutex_unlock(&self->lock); 
		return -1; 
	}
	// this just dumps the returned object
	lua_pushnil(self->lua); 
	const char *k; 
	blob_offset_t root = blob_open_table(&self->signature); 
	while(lua_next(self->lua, -2)){
		lua_pop(self->lua, 1); 
		k = lua_tostring(self->lua, -1); 
		blob_put_string(&self->signature, k); 
		blob_offset_t m = blob_open_array(&self->signature); 
		blob_close_array(&self->signature, m); 
	}
	blob_close_table(&self->signature, root); 

	pthread_mutex_unlock(&self->lock); 
	return 0; 
}

int orange_luaobject_call(struct orange_luaobject *self, struct orange_session *session, const char *method, const struct blob_field *in, struct blob *out){
	if(!self || !self->lua) return -1; 

	pthread_mutex_lock(&self->lock); 
	// set self pointer of the global lua session object to point to current session
	orange_lua_set_session(self->lua, session); 

	// first item on lua stack should always be the table that was returned when the file was run at load time
	if(lua_type(self->lua, -1) != LUA_TTABLE) {
		ERROR("lua state is broken. No table on stack!\n"); 

		blob_put_string(out, "error"); 
		blob_offset_t t = blob_open_table(out); 
		blob_put_string(out, "str"); 
		blob_put_string(out, "Lua backend state is broken! This should never happen!"); 
		blob_put_string(out, "code"); 
		blob_put_int(out, lua_tointeger(self->lua, -1));
		blob_close_table(out, t); 

		pthread_mutex_unlock(&self->lock); 
		return -1; 
	}

	// we get the field indexed by supplied method from our lua object
	lua_getfield(self->lua, -1, method); 
	if(!lua_isfunction(self->lua, -1)){
		ERROR("can not call %s on %s: field is not a function!\n", method, self->name); 

		blob_put_string(out, "error"); 
		blob_offset_t t = blob_open_table(out); 
		blob_put_string(out, "str"); 
		blob_put_string(out, "Not a function"); 
		blob_put_string(out, "code"); 
		blob_put_int(out, lua_tointeger(self->lua, -1));
		blob_close_table(out, t); 

		pthread_mutex_unlock(&self->lock); 
		return -1; 
	}

	// arguments are either supplied by the user or an empty table
	if(in) orange_lua_blob_to_table(self->lua, in, true); 
	else lua_newtable(self->lua); 

	// call the method that we previously poped out of the table
	if(lua_pcall(self->lua, 1, 1, 0) != 0){
		ERROR("error calling %s: %s\n", method, lua_tostring(self->lua, -1)); 
		blob_put_string(out, "error"); 
		blob_offset_t t = blob_open_table(out); 
		blob_put_string(out, "str"); 
		char error_msg[255];
		sprintf(error_msg,"error calling %s: %s\n", method, lua_tostring(self->lua, -1));
		blob_put_string(out, error_msg);
		blob_put_string(out, "LUA error in backend function"); 
		blob_put_string(out, "code"); 
		blob_put_int(out, lua_tointeger(self->lua, -1));
		blob_close_table(out, t); 
		pthread_mutex_unlock(&self->lock); 
		return -1; 
	}

	// support both table response and an error code response. 
	// if lua method returns a number then it is always treated as an error code
	if(lua_type(self->lua, -1) == LUA_TTABLE) {
		blob_put_string(out, "result"); 
		blob_offset_t t = blob_open_table(out); 
		orange_lua_table_to_blob(self->lua, out, true); 
		blob_close_table(out, t); 
	} else if(lua_type(self->lua, -1) == LUA_TNUMBER){
		blob_put_string(out, "error"); 
		blob_offset_t t = blob_open_table(out); 
		blob_put_string(out, "code"); 
		blob_put_int(out, lua_tointeger(self->lua, -1));
		blob_close_table(out, t); 
	} else {
		// for all other return types return an empty object
		blob_put_string(out, "result"); 
		blob_offset_t t = blob_open_table(out); 
		blob_close_table(out, t); 
	}

	lua_pop(self->lua, 1); 	
	
	pthread_mutex_unlock(&self->lock); 
	return 0; 
}


