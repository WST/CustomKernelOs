CC = cc -DHAVE_CONFIG_H -I. -Iinclude -Ilib -Ibuiltins

/ext/bin/bash: a01.a a02.a a03.a a04.a a05.a a06.a a07.a a08.a a09.a a10.a a11.a a12.a a13.a a14.a a15.a a16.a a17.a a18.a a19.a a20.a a21.a a22.a a23.a a24.a a25.a a26.a
	$(CC) -c shell.c -o obj/t_shell.o
	cc -o /ext/bin/bash obj/t_shell.o a01.a a02.a a03.a a04.a a05.a a06.a a07.a a08.a a09.a a10.a a11.a a12.a a13.a a14.a a15.a a16.a a17.a a18.a a19.a a20.a a21.a a22.a a23.a a24.a a25.a a26.a

obj/mkbuiltins:
	$(CC) -o obj/mkbuiltins builtins/mkbuiltins.c

obj/mksyntax:
	$(CC) -o obj/mksyntax mksyntax.c

obj/mksignames:
	$(CC) -DBUILDTOOL -o obj/mksignames support/mksignames.c support/signames.c

obj/genb:
	$(CC) -o obj/genb genbuiltins.c

obj/defs.ok: obj/mkbuiltins obj/genb
	obj/genb
	echo ok > obj/defs.ok

syntax.c: obj/mksyntax
	obj/mksyntax -o syntax.c

signames.h: obj/mksignames
	obj/mksignames signames.h

a01.a: obj/defs.ok syntax.c signames.h
	$(CC) -c eval.c -o obj/t_eval.o
	$(CC) -c y.tab.c -o obj/t_y.tab.o
	$(CC) -c general.c -o obj/t_general.o
	$(CC) -c make_cmd.c -o obj/t_make_cmd.o
	$(CC) -c print_cmd.c -o obj/t_print_cmd.o
	$(CC) -c dispose_cmd.c -o obj/t_dispose_cmd.o
	ar rc a01.a obj/t_eval.o obj/t_y.tab.o obj/t_general.o obj/t_make_cmd.o obj/t_print_cmd.o obj/t_dispose_cmd.o

a02.a: obj/defs.ok syntax.c signames.h
	$(CC) -c execute_cmd.c -o obj/t_execute_cmd.o
	$(CC) -c variables.c -o obj/t_variables.o
	$(CC) -c copy_cmd.c -o obj/t_copy_cmd.o
	$(CC) -c error.c -o obj/t_error.o
	$(CC) -c expr.c -o obj/t_expr.o
	$(CC) -c flags.c -o obj/t_flags.o
	ar rc a02.a obj/t_execute_cmd.o obj/t_variables.o obj/t_copy_cmd.o obj/t_error.o obj/t_expr.o obj/t_flags.o

a03.a: obj/defs.ok syntax.c signames.h
	$(CC) -c nojobs.c -o obj/t_nojobs.o
	$(CC) -c subst.c -o obj/t_subst.o
	$(CC) -c hashcmd.c -o obj/t_hashcmd.o
	$(CC) -c hashlib.c -o obj/t_hashlib.o
	$(CC) -c mailcheck.c -o obj/t_mailcheck.o
	$(CC) -c trap.c -o obj/t_trap.o
	ar rc a03.a obj/t_nojobs.o obj/t_subst.o obj/t_hashcmd.o obj/t_hashlib.o obj/t_mailcheck.o obj/t_trap.o

a04.a: obj/defs.ok syntax.c signames.h
	$(CC) -c input.c -o obj/t_input.o
	$(CC) -c unwind_prot.c -o obj/t_unwind_prot.o
	$(CC) -c pathexp.c -o obj/t_pathexp.o
	$(CC) -c sig.c -o obj/t_sig.o
	$(CC) -c test.c -o obj/t_test.o
	$(CC) -c version.c -o obj/t_version.o
	ar rc a04.a obj/t_input.o obj/t_unwind_prot.o obj/t_pathexp.o obj/t_sig.o obj/t_test.o obj/t_version.o

a05.a: obj/defs.ok syntax.c signames.h
	$(CC) -c alias.c -o obj/t_alias.o
	$(CC) -c array.c -o obj/t_array.o
	$(CC) -c arrayfunc.c -o obj/t_arrayfunc.o
	$(CC) -c assoc.c -o obj/t_assoc.o
	$(CC) -c braces.c -o obj/t_braces.o
	$(CC) -c bracecomp.c -o obj/t_bracecomp.o
	ar rc a05.a obj/t_alias.o obj/t_array.o obj/t_arrayfunc.o obj/t_assoc.o obj/t_braces.o obj/t_bracecomp.o

a06.a: obj/defs.ok syntax.c signames.h
	$(CC) -c bashhist.c -o obj/t_bashhist.o
	$(CC) -c bashline.c -o obj/t_bashline.o
	$(CC) -c list.c -o obj/t_list.o
	$(CC) -c stringlib.c -o obj/t_stringlib.o
	$(CC) -c locale.c -o obj/t_locale.o
	$(CC) -c findcmd.c -o obj/t_findcmd.o
	ar rc a06.a obj/t_bashhist.o obj/t_bashline.o obj/t_list.o obj/t_stringlib.o obj/t_locale.o obj/t_findcmd.o

a07.a: obj/defs.ok syntax.c signames.h
	$(CC) -c redir.c -o obj/t_redir.o
	$(CC) -c pcomplete.c -o obj/t_pcomplete.o
	$(CC) -c pcomplib.c -o obj/t_pcomplib.o
	$(CC) -c xmalloc.c -o obj/t_xmalloc.o
	$(CC) -c syntax.c -o obj/t_syntax.o
	$(CC) -c builtins/alias.c -o obj/b_alias.o
	ar rc a07.a obj/t_redir.o obj/t_pcomplete.o obj/t_pcomplib.o obj/t_xmalloc.o obj/t_syntax.o obj/b_alias.o

a08.a: obj/defs.ok syntax.c signames.h
	$(CC) -c builtins/bashgetopt.c -o obj/b_bashgetopt.o
	$(CC) -c builtins/bind.c -o obj/b_bind.o
	$(CC) -c builtins/break.c -o obj/b_break.o
	$(CC) -c builtins/builtin.c -o obj/b_builtin.o
	$(CC) -c builtins/builtins.c -o obj/b_builtins.o
	$(CC) -c builtins/caller.c -o obj/b_caller.o
	ar rc a08.a obj/b_bashgetopt.o obj/b_bind.o obj/b_break.o obj/b_builtin.o obj/b_builtins.o obj/b_caller.o

a09.a: obj/defs.ok syntax.c signames.h
	$(CC) -c builtins/cd.c -o obj/b_cd.o
	$(CC) -c builtins/colon.c -o obj/b_colon.o
	$(CC) -c builtins/command.c -o obj/b_command.o
	$(CC) -c builtins/common.c -o obj/b_common.o
	$(CC) -c builtins/complete.c -o obj/b_complete.o
	$(CC) -c builtins/declare.c -o obj/b_declare.o
	ar rc a09.a obj/b_cd.o obj/b_colon.o obj/b_command.o obj/b_common.o obj/b_complete.o obj/b_declare.o

a10.a: obj/defs.ok syntax.c signames.h
	$(CC) -c builtins/echo.c -o obj/b_echo.o
	$(CC) -c builtins/enable.c -o obj/b_enable.o
	$(CC) -c builtins/eval.c -o obj/b_eval.o
	$(CC) -c builtins/evalfile.c -o obj/b_evalfile.o
	$(CC) -c builtins/evalstring.c -o obj/b_evalstring.o
	$(CC) -c builtins/exec.c -o obj/b_exec.o
	ar rc a10.a obj/b_echo.o obj/b_enable.o obj/b_eval.o obj/b_evalfile.o obj/b_evalstring.o obj/b_exec.o

a11.a: obj/defs.ok syntax.c signames.h
	$(CC) -c builtins/exit.c -o obj/b_exit.o
	$(CC) -c builtins/fc.c -o obj/b_fc.o
	$(CC) -c builtins/fg_bg.c -o obj/b_fg_bg.o
	$(CC) -c builtins/getopt.c -o obj/b_getopt.o
	$(CC) -c builtins/getopts.c -o obj/b_getopts.o
	$(CC) -c builtins/hash.c -o obj/b_hash.o
	ar rc a11.a obj/b_exit.o obj/b_fc.o obj/b_fg_bg.o obj/b_getopt.o obj/b_getopts.o obj/b_hash.o

a12.a: obj/defs.ok syntax.c signames.h
	$(CC) -c builtins/help.c -o obj/b_help.o
	$(CC) -c builtins/history.c -o obj/b_history.o
	$(CC) -c builtins/jobs.c -o obj/b_jobs.o
	$(CC) -c builtins/kill.c -o obj/b_kill.o
	$(CC) -c builtins/let.c -o obj/b_let.o
	$(CC) -c builtins/mapfile.c -o obj/b_mapfile.o
	ar rc a12.a obj/b_help.o obj/b_history.o obj/b_jobs.o obj/b_kill.o obj/b_let.o obj/b_mapfile.o

a13.a: obj/defs.ok syntax.c signames.h
	$(CC) -c builtins/printf.c -o obj/b_printf.o
	$(CC) -c builtins/pushd.c -o obj/b_pushd.o
	$(CC) -c builtins/read.c -o obj/b_read.o
	$(CC) -c builtins/return.c -o obj/b_return.o
	$(CC) -c builtins/set.c -o obj/b_set.o
	$(CC) -c builtins/setattr.c -o obj/b_setattr.o
	ar rc a13.a obj/b_printf.o obj/b_pushd.o obj/b_read.o obj/b_return.o obj/b_set.o obj/b_setattr.o

a14.a: obj/defs.ok syntax.c signames.h
	$(CC) -c builtins/shift.c -o obj/b_shift.o
	$(CC) -c builtins/shopt.c -o obj/b_shopt.o
	$(CC) -c builtins/source.c -o obj/b_source.o
	$(CC) -c builtins/suspend.c -o obj/b_suspend.o
	$(CC) -c builtins/test.c -o obj/b_test.o
	$(CC) -c builtins/times.c -o obj/b_times.o
	ar rc a14.a obj/b_shift.o obj/b_shopt.o obj/b_source.o obj/b_suspend.o obj/b_test.o obj/b_times.o

a15.a: obj/defs.ok syntax.c signames.h
	$(CC) -c builtins/trap.c -o obj/b_trap.o
	$(CC) -c builtins/type.c -o obj/b_type.o
	$(CC) -c builtins/ulimit.c -o obj/b_ulimit.o
	$(CC) -c builtins/umask.c -o obj/b_umask.o
	$(CC) -c builtins/wait.c -o obj/b_wait.o
	$(CC) -c lib/glob/glob.c -o obj/g_glob.o
	ar rc a15.a obj/b_trap.o obj/b_type.o obj/b_ulimit.o obj/b_umask.o obj/b_wait.o obj/g_glob.o

a16.a: obj/defs.ok syntax.c signames.h
	$(CC) -c lib/glob/strmatch.c -o obj/g_strmatch.o
	$(CC) -c lib/glob/smatch.c -o obj/g_smatch.o
	$(CC) -c lib/glob/gmisc.c -o obj/g_gmisc.o
	$(CC) -c lib/glob/xmbsrtowcs.c -o obj/g_xmbsrtowcs.o
	$(CC) -c lib/tilde/tilde.c -o obj/u_tilde.o
	$(CC) -c lib/sh/clktck.c -o obj/s_clktck.o
	ar rc a16.a obj/g_strmatch.o obj/g_smatch.o obj/g_gmisc.o obj/g_xmbsrtowcs.o obj/u_tilde.o obj/s_clktck.o

a17.a: obj/defs.ok syntax.c signames.h
	$(CC) -c lib/sh/clock.c -o obj/s_clock.o
	$(CC) -c lib/sh/getenv.c -o obj/s_getenv.o
	$(CC) -c lib/sh/oslib.c -o obj/s_oslib.o
	$(CC) -c lib/sh/setlinebuf.c -o obj/s_setlinebuf.o
	$(CC) -c lib/sh/strnlen.c -o obj/s_strnlen.o
	$(CC) -c lib/sh/itos.c -o obj/s_itos.o
	ar rc a17.a obj/s_clock.o obj/s_getenv.o obj/s_oslib.o obj/s_setlinebuf.o obj/s_strnlen.o obj/s_itos.o

a18.a: obj/defs.ok syntax.c signames.h
	$(CC) -c lib/sh/zread.c -o obj/s_zread.o
	$(CC) -c lib/sh/zwrite.c -o obj/s_zwrite.o
	$(CC) -c lib/sh/shtty.c -o obj/s_shtty.o
	$(CC) -c lib/sh/shmatch.c -o obj/s_shmatch.o
	$(CC) -c lib/sh/eaccess.c -o obj/s_eaccess.o
	$(CC) -c lib/sh/netconn.c -o obj/s_netconn.o
	ar rc a18.a obj/s_zread.o obj/s_zwrite.o obj/s_shtty.o obj/s_shmatch.o obj/s_eaccess.o obj/s_netconn.o

a19.a: obj/defs.ok syntax.c signames.h
	$(CC) -c lib/sh/netopen.c -o obj/s_netopen.o
	$(CC) -c lib/sh/timeval.c -o obj/s_timeval.o
	$(CC) -c lib/sh/makepath.c -o obj/s_makepath.o
	$(CC) -c lib/sh/pathcanon.c -o obj/s_pathcanon.o
	$(CC) -c lib/sh/pathphys.c -o obj/s_pathphys.o
	$(CC) -c lib/sh/tmpfile.c -o obj/s_tmpfile.o
	ar rc a19.a obj/s_netopen.o obj/s_timeval.o obj/s_makepath.o obj/s_pathcanon.o obj/s_pathphys.o obj/s_tmpfile.o

a20.a: obj/defs.ok syntax.c signames.h
	$(CC) -c lib/sh/stringlist.c -o obj/s_stringlist.o
	$(CC) -c lib/sh/stringvec.c -o obj/s_stringvec.o
	$(CC) -c lib/sh/spell.c -o obj/s_spell.o
	$(CC) -c lib/sh/shquote.c -o obj/s_shquote.o
	$(CC) -c lib/sh/strtrans.c -o obj/s_strtrans.o
	$(CC) -c lib/sh/snprintf.c -o obj/s_snprintf.o
	ar rc a20.a obj/s_stringlist.o obj/s_stringvec.o obj/s_spell.o obj/s_shquote.o obj/s_strtrans.o obj/s_snprintf.o

a21.a: obj/defs.ok syntax.c signames.h
	$(CC) -c lib/sh/mailstat.c -o obj/s_mailstat.o
	$(CC) -c lib/sh/fmtulong.c -o obj/s_fmtulong.o
	$(CC) -c lib/sh/fmtullong.c -o obj/s_fmtullong.o
	$(CC) -c lib/sh/fmtumax.c -o obj/s_fmtumax.o
	$(CC) -c lib/sh/zcatfd.c -o obj/s_zcatfd.o
	$(CC) -c lib/sh/zmapfd.c -o obj/s_zmapfd.o
	ar rc a21.a obj/s_mailstat.o obj/s_fmtulong.o obj/s_fmtullong.o obj/s_fmtumax.o obj/s_zcatfd.o obj/s_zmapfd.o

a22.a: obj/defs.ok syntax.c signames.h
	$(CC) -c lib/sh/winsize.c -o obj/s_winsize.o
	$(CC) -c lib/sh/wcsdup.c -o obj/s_wcsdup.o
	$(CC) -c lib/sh/fpurge.c -o obj/s_fpurge.o
	$(CC) -c lib/sh/zgetline.c -o obj/s_zgetline.o
	$(CC) -c lib/sh/mbscmp.c -o obj/s_mbscmp.o
	$(CC) -c lib/sh/mbsncmp.c -o obj/s_mbsncmp.o
	ar rc a22.a obj/s_winsize.o obj/s_wcsdup.o obj/s_fpurge.o obj/s_zgetline.o obj/s_mbscmp.o obj/s_mbsncmp.o

a23.a: obj/defs.ok syntax.c signames.h
	$(CC) -c lib/sh/uconvert.c -o obj/s_uconvert.o
	$(CC) -c lib/sh/ufuncs.c -o obj/s_ufuncs.o
	$(CC) -c lib/sh/casemod.c -o obj/s_casemod.o
	$(CC) -c lib/sh/input_avail.c -o obj/s_input_avail.o
	$(CC) -c lib/sh/mbscasecmp.c -o obj/s_mbscasecmp.o
	$(CC) -c lib/sh/fnxform.c -o obj/s_fnxform.o
	ar rc a23.a obj/s_uconvert.o obj/s_ufuncs.o obj/s_casemod.o obj/s_input_avail.o obj/s_mbscasecmp.o obj/s_fnxform.o

a24.a: obj/defs.ok syntax.c signames.h
	$(CC) -c lib/sh/unicode.c -o obj/s_unicode.o
	$(CC) -c lib/sh/shmbchar.c -o obj/s_shmbchar.o
	$(CC) -c lib/sh/strvis.c -o obj/s_strvis.o
	$(CC) -c lib/sh/strscpy.c -o obj/s_strscpy.o
	$(CC) -c lib/sh/anonfile.c -o obj/s_anonfile.o
	$(CC) -c lib/sh/compat.c -o obj/s_compat.o
	ar rc a24.a obj/s_unicode.o obj/s_shmbchar.o obj/s_strvis.o obj/s_strscpy.o obj/s_anonfile.o obj/s_compat.o

a25.a: obj/defs.ok syntax.c signames.h
	$(CC) -c lib/sh/utf8.c -o obj/s_utf8.o
	$(CC) -c lib/sh/random.c -o obj/s_random.o
	$(CC) -c lib/sh/gettimeofday.c -o obj/s_gettimeofday.o
	$(CC) -c lib/sh/timers.c -o obj/s_timers.o
	$(CC) -c lib/sh/wcsnwidth.c -o obj/s_wcsnwidth.o
	ar rc a25.a obj/s_utf8.o obj/s_random.o obj/s_gettimeofday.o obj/s_timers.o obj/s_wcsnwidth.o

a26.a: obj/defs.ok syntax.c signames.h
	$(CC) -c lib/sh/mbschr.c -o obj/s_mbschr.o
	ar rc a26.a obj/s_mbschr.o
