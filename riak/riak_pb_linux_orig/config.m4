PHP_ARG_ENABLE([riak_pb],
  [whether to enable the Riak PB extension],
  [AS_HELP_STRING([--enable-riak-pb], [Enable the Linux Riak PB file client])],
  [no])

if test "$PHP_RIAK_PB" != "no"; then
  AC_CHECK_HEADERS([poll.h netdb.h sys/socket.h], [], [
    AC_MSG_ERROR([riak_pb requires POSIX networking headers])
  ])
  AC_CHECK_FUNCS([clock_gettime poll], [], [
    AC_MSG_ERROR([riak_pb requires clock_gettime and poll])
  ])
  PHP_NEW_EXTENSION([riak_pb], [riak_pb.c pb_codec.c riak_transport.c], [$ext_shared])
fi
