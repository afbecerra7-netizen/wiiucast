SPIKES := s1-multicast s2-decode s3-throughput

.PHONY: all clean $(SPIKES)

all: $(SPIKES)

$(SPIKES):
	@$(MAKE) --no-print-directory -C $@

clean:
	@for d in $(SPIKES); do $(MAKE) --no-print-directory -C $$d clean; done
