#ifndef PHP_RIAK_PB_H
#define PHP_RIAK_PB_H

#include "php.h"

#if PHP_VERSION_ID < 80400 || PHP_VERSION_ID >= 80500
#error "This extension currently targets PHP 8.4."
#endif

#define PHP_RIAK_PB_VERSION "0.1.0"
extern zend_module_entry riak_pb_module_entry;
#define phpext_riak_pb_ptr &riak_pb_module_entry

#endif
