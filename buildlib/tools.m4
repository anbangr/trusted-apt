AC_DEFUN([ah_HAVE_GETCONF],
	[AC_ARG_WITH(getconf,
		[  --with-getconf          Enable automagical buildtime configuration],
		[if test "$withval" = "yes"; then
			AC_PATH_PROG(GETCONF, getconf)
		elif test ! "$withval" = "no";then 
			AC_MSG_CHECKING([getconf])
			AC_MSG_RESULT([$withval])
			GETCONF=$withval
		fi],
		[AC_PATH_PROG(GETCONF, getconf)]
	)
	AC_SUBST(GETCONF)
])

dnl ah_GET_CONF(variable, value ..., [default])
AC_DEFUN([ah_GET_GETCONF],
	[AC_REQUIRE([ah_HAVE_GETCONF])
	if test ! -z "$GETCONF";then
		old_args="[$]@"
		set -- $2
		while eval test -z \"\$$1\" -a ! -z \"[$]1\";do
			eval $1=`$GETCONF "[$]1" 2>/dev/null`
			shift
		done
	fi
	if eval test -z \"\$$1\" -o \"\$$1\" = "-1";then
		eval $1="$3"
	fi
])
AC_DEFUN([ah_NUM_CPUS],
	[AC_MSG_CHECKING([number of cpus])
	AC_ARG_WITH(cpus,
		[  --with-cpus             The number of cpus to be used for building(see --with-procs, default 1)],
		[
		if test "$withval" = "yes"; then
			ah_GET_GETCONF(NUM_CPUS, SC_NPROCESSORS_ONLN _NPROCESSORS_ONLN, 1)
		elif test ! "$withval" = "no";then
			NUM_CPUS=$withval
		elif test "$withval" = "no";then
			NUM_CPUS=1
		fi],
		[ah_GET_GETCONF(NUM_CPUS, SC_NPROCESSORS_ONLN _NPROCESSORS_ONLN, 1)]
	)
	ah_NUM_CPUS_msg="$NUM_CPUS"
	if test "$NUM_CPUS" = "0"; then
		# broken getconf, time to bitch.
		ah_NUM_CPUS_msg="found 0 cpus.  Has someone done a lobotomy?"
		NUM_CPUS=1
	fi
	if test $NUM_CPUS = 1 ;then
		default_PROC_MULTIPLY=1
	else
		default_PROC_MULTIPLY=2
	fi
	AC_MSG_RESULT([$ah_NUM_CPUS_msg])
	AC_SUBST(NUM_CPUS)
])
AC_DEFUN([ah_PROC_MULTIPLY],
	[AC_REQUIRE([ah_NUM_CPUS])
	AC_MSG_CHECKING([processor multiplier])
	AC_ARG_WITH(proc-multiply,
		[  --with-proc-multiply    Multiply this * number of cpus for parallel making(default 2).],
		[if test "$withval" = "yes"; then
			PROC_MULTIPLY=$default_PROC_MULTIPLY
		elif test ! "$withval" = "no";then
			PROC_MULTIPLY=$withval
		fi],
		[PROC_MULTIPLY=$default_PROC_MULTIPLY]
	)
	AC_MSG_RESULT([$PROC_MULTIPLY])
	AC_SUBST(PROC_MULTIPLY)
])

AC_DEFUN([ah_NUM_PROCS],
	[AC_REQUIRE([ah_PROC_MULTIPLY])
	AC_REQUIRE([ah_NUM_CPUS])
	AC_MSG_CHECKING([number of processes to run during make])
	AC_ARG_WITH(procs,
		[  --with-procs            The number of processes to run in parallel during make(num_cpus * multiplier).],
		[if test "$withval" = "yes"; then
			NUM_PROCS=`expr $NUM_CPUS \* $PROC_MULTIPLY`
		elif test ! "$withval" = "no";then
			NUM_PROCS=$withval
		fi],
		[NUM_PROCS=`expr $NUM_CPUS \* $PROC_MULTIPLY`]
	)
	AC_MSG_RESULT([$NUM_PROCS])
	AC_SUBST(NUM_PROCS)
])

AC_DEFUN([ah_GCC3DEP],[
	AC_MSG_CHECKING(if $CXX -MD works)
	touch gcc3dep.cc
	${CXX-c++} -MD -o gcc3dep_test.o -c gcc3dep.cc
	rm -f gcc3dep.cc gcc3dep_test.o
	if test -e gcc3dep.d; then
		rm -f gcc3dep.d
		GCC_MD=input
		GCC3DEP=
	elif test -e gcc3dep_test.d; then
		rm -f gcc3dep_test.d
		GCC_MD=output
		GCC3DEP=yes
	else
		AC_MSG_ERROR(no)
	fi
	AC_MSG_RESULT([yes, for $GCC_MD])
	AC_SUBST(GCC3DEP)
])
