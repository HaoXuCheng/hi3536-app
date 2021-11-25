AUTOMAKE_OPTIONS=foreign

lib_LTLIBRARIES=libhi3536-vdec.la
libhi3536_vdec_la_SOURCES=hi3536-vdec.c sample_comm_vo.c sample_comm_vpss.c sample_comm_sys.c sample_comm_vdec.c
libhi3536_vdec_la_LDFLAGS=-lmpi -lhdmi -ljpeg -ldnvqe -lVoiceEngine -lupvqe

#testdir=$(prefix)/etc/tea/test
#test_DATA=hi3536_vdec.xml.m

#schemadir=$(prefix)/etc/tea/schema
#schema_DATA=hi3536_vdec.s

initdir=$(prefix)/etc/rc.init.d
init_SCRIPTS=03load3536
