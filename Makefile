SUBDIRS = src
ODIR = build
MKDIR_P = mkdir -p


.PHONY: all
all: makedirs subdirs

subdirs:
	for dir in $(SUBDIRS); do \
	   $(MAKE) -C $$dir; \
	done

makedirs: $(ODIR)
	
${ODIR}:
	${MKDIR_P} ${ODIR}
	
cleanAll:
	rm -f $(ODIR)/*