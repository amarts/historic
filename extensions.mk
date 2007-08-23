# Copyright (C) 2006 Z RESEARCH Inc. <http://www.zresearch.com>
#  
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#  
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#  
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
#  

APPLICATIONS = GlusterHPC GlusterEP self-install shutdown

HPC_PACKAGES = \
	mpich2-pkg \
	torque-pkg \
	slurm-pkg \
	autologin-pkg \
	pdsh-pkg \
	c3-pkg \
	ganglia-pkg \
	hpc-base-pkg \
	lam-pkg \
	ofed-pkg 
#	cerebro-pkg \
#	genders-pkg \
#	conman-pkg \
#	powerman-pkg 



EXTENSIONS= \
	init \
	bootdisk-manager \
	bootp-manager \
	bootloader-manager \
	client-boot \
	master-boot \
	client-reg \
	iface-cfg \
	mac-collector \
	fs-utils \
	gutils \
	auth \
	net-utils \
        debug-utils \
	$(HPC_PACKAGES) \
	$(APPLICATIONS)
#	freeipmi \
#	bootloader-manager

%.stage3: %.live
	cd $(BUILD_LIVE)/$(PACKAGE_NAME) ; \
	export $(LIVE_INSTALL_ENV) ; \
	$(call LIVE_INSTALL_CMD,$(DESTDIR_STAGE3)/$(EXTENSION)/destdir_$(ARCH))


include $(EXTENSIONS:%=$(top_srcdir)/extensions/%.mk)

.PHONY: extensions

extensions: $(EXTENSIONS:%=%.gex)

copy-extensions:
	mkdir -p $(top_builddir)/iso_fs_$(ARCH)/extensions
	cp $(DESTDIR_STAGE3)/*.{gex,tgz} $(top_builddir)/iso_fs_$(ARCH)/extensions


$(EXTENSIONS:%=%.gex):
	mkdir -p $(BUILD_LIVE)/$(EXTENSION).gex
	cd $(BUILD_LIVE)/$(EXTENSION).gex && \
		$(abs_top_srcdir)/extensions/$(EXTENSION)/configure \
		--prefix=/usr --host=$(CROSS) --build=$(GLUSTER_BUILD)
	$(MAKE) -C $(BUILD_LIVE)/$(EXTENSION).gex all
	$(MAKE) -C $(BUILD_LIVE)/$(EXTENSION).gex install \
		DESTDIR=$(DESTDIR_LIVE)
	mkdir -p $(DESTDIR_STAGE3)/$(EXTENSION)/destdir_$(ARCH)
	$(MAKE) -C $(BUILD_LIVE)/$(EXTENSION).gex install \
		DESTDIR=$(DESTDIR_STAGE3)/$(EXTENSION)/destdir_$(ARCH)
	cp $(abs_top_srcdir)/extensions/$(EXTENSION)/runme \
		$(DESTDIR_STAGE3)/$(EXTENSION)
	cp $(abs_top_srcdir)/extensions/$(EXTENSION).gex \
		$(DESTDIR_STAGE3)/$(EXTENSION).gex
	$(abs_top_srcdir)/cleanup.sh $(DESTDIR_STAGE3)/$(EXTENSION)/destdir_$(ARCH) \
		STRIP=$(CROSS)-strip
	tar -C $(DESTDIR_STAGE3)/$(EXTENSION) \
		-czf $(DESTDIR_STAGE3)/$(EXTENSION).tgz .

clean.extensions:
	rm -rf destdir_stage3

clean: clean.extensions