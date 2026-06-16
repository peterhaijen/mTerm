BUILD_DIR := build/release
DEBUG_BUILD_DIR := build/debug
DEB_BUILD_DIR := build/deb
DEB_OUTPUT_DIR := build/packages
GENERATOR ?= Ninja

.PHONY: help release debug deb run tag push clean clean-release clean-debug clean-deb distclean

help:
	@echo "Available targets:"
	@echo "  release        Configure and build a release binary in $(BUILD_DIR)"
	@echo "  debug          Configure and build a debug binary with symbols in $(DEBUG_BUILD_DIR)"
	@echo "  deb            Build an OS-specific Debian package in $(DEB_OUTPUT_DIR)"
	@echo "  run            Run the most recently built mTerm executable"
	@echo "  tag            Create a new release tag"
	@echo "  push           Push the current branch and tags"
	@echo "  clean          Remove release build outputs"
	@echo "  clean-release  Alias for clean"
	@echo "  clean-debug    Remove debug build outputs"
	@echo "  clean-deb      Remove Debian package build outputs"
	@echo "  distclean      Remove all build directories"

release:
	cmake -S . -B $(BUILD_DIR) -G "$(GENERATOR)" -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD_DIR)

debug:
	cmake -S . -B $(DEBUG_BUILD_DIR) -G "$(GENERATOR)" -DCMAKE_BUILD_TYPE=Debug
	cmake --build $(DEBUG_BUILD_DIR)

deb: release
	@set -e; \
	. /etc/os-release; \
	os_release="$${VERSION_CODENAME:-$${UBUNTU_CODENAME:-unknown}}"; \
	os_release=$$(printf "%s" "$$os_release" | sed 's/[^A-Za-z0-9.+:~-]/+/g'); \
	version=$$(sed -n 's/^#define MTERM_VERSION "\(.*\)"/\1/p' "$(BUILD_DIR)/generated/version.h"); \
	if [ -z "$$version" ]; then \
		echo "Could not read version from $(BUILD_DIR)/generated/version.h" >&2; \
		exit 1; \
	fi; \
	pkg_version="$${version}-1~$${os_release}1"; \
	arch=$$(dpkg --print-architecture); \
	stage="$(DEB_BUILD_DIR)/mterm"; \
	rm -rf "$$stage"; \
	mkdir -p "$$stage/DEBIAN" "$$stage/usr/bin" \
		"$$stage/usr/share/applications" \
		"$$stage/usr/share/icons/hicolor/scalable/apps" \
		"$(DEB_OUTPUT_DIR)"; \
	cp "$(BUILD_DIR)/mTerm" "$$stage/usr/bin/mTerm"; \
	cp data/mterm.desktop "$$stage/usr/share/applications/mterm.desktop"; \
	cp assets/mterm.svg "$$stage/usr/share/icons/hicolor/scalable/apps/mterm.svg"; \
	chmod 0755 "$$stage/usr/bin/mTerm"; \
	write_control() { \
		{ \
			echo "Package: mterm"; \
			echo "Version: $$pkg_version"; \
			echo "Section: utils"; \
			echo "Priority: optional"; \
			echo "Architecture: $$arch"; \
			echo "Maintainer: Peter <peter@localhost>"; \
			echo "Depends: $$1"; \
			echo "Description: Qt/QTermWidget terminal manager that broadcasts commands"; \
			echo " mTerm broadcasts commands to multiple terminal sessions."; \
		} > "$$stage/DEBIAN/control"; \
	}; \
	write_control "libc6"; \
	if ! command -v dpkg-shlibdeps >/dev/null 2>&1; then \
		echo "dpkg-shlibdeps is required to generate package dependencies." >&2; \
		echo "Install dpkg-dev and retry." >&2; \
		exit 1; \
	fi; \
	mkdir -p "$(DEB_BUILD_DIR)/substvars" debian; \
	{ \
		echo "Source: mterm"; \
		echo "Section: utils"; \
		echo "Priority: optional"; \
		echo "Maintainer: Peter <peter@localhost>"; \
		echo "Standards-Version: 4.6.2"; \
		echo ""; \
		echo "Package: mterm"; \
		echo "Architecture: $$arch"; \
		echo "Depends: \$${shlibs:Depends}"; \
		echo "Description: Qt/QTermWidget terminal manager that broadcasts commands"; \
		echo " mTerm broadcasts commands to multiple terminal sessions."; \
	} > debian/control; \
	dpkg-shlibdeps -O "$$stage/usr/bin/mTerm" > "$(DEB_BUILD_DIR)/substvars/shlibs"; \
	rm -f debian/control; \
	rmdir debian 2>/dev/null || true; \
	depends=$$(sed -n 's/^shlibs:Depends=//p' "$(DEB_BUILD_DIR)/substvars/shlibs"); \
	if [ -z "$$depends" ]; then \
		echo "dpkg-shlibdeps did not return any dependencies." >&2; \
		exit 1; \
	fi; \
	write_control "$$depends"; \
	dpkg-deb --root-owner-group --build "$$stage" "$(DEB_OUTPUT_DIR)/mterm_$${pkg_version}_$${arch}.deb"; \
	echo "Built $(DEB_OUTPUT_DIR)/mterm_$${pkg_version}_$${arch}.deb"

run:
	@exe=$$(find build build-* build_* -type f -name mTerm -perm -111 -printf '%T@ %p\n' 2>/dev/null | sort -n | tail -1 | cut -d' ' -f2-); \
	if [ -z "$$exe" ]; then \
		echo "No built mTerm executable found under build directories." >&2; \
		exit 1; \
	fi; \
	echo "Running $$exe"; \
	"$$exe"

tag:
	@latest=$$(git tag --list 'v[0-9]*.[0-9]*.[0-9]*' --sort=-v:refname | head -1); \
	if [ -z "$$latest" ]; then \
		echo "No vX.Y.Z tag found." >&2; \
		exit 1; \
	fi; \
	version=$${latest#v}; \
	major=$$(printf '%s\n' "$$version" | cut -d. -f1); \
	minor=$$(printf '%s\n' "$$version" | cut -d. -f2); \
	patch=$$(printf '%s\n' "$$version" | cut -d. -f3); \
	patch_tag="v$$major.$$minor.$$((patch + 1))"; \
	minor_tag="v$$major.$$((minor + 1)).0"; \
	major_tag="v$$((major + 1)).0.0"; \
	echo "Last tag: $$latest"; \
	echo "1) $$patch_tag (default)"; \
	echo "2) $$minor_tag"; \
	echo "3) $$major_tag"; \
	printf "Select tag [1]: "; \
	read choice; \
	case "$$choice" in \
		""|1) new_tag="$$patch_tag" ;; \
		2) new_tag="$$minor_tag" ;; \
		3) new_tag="$$major_tag" ;; \
		*) echo "Invalid choice: $$choice" >&2; exit 1 ;; \
	esac; \
	git tag -a "$$new_tag" -m "Release $$new_tag"; \
	echo "Created tag $$new_tag"

push:
	git push
	git push --tags

clean: clean-release

clean-release:
	rm -rf $(BUILD_DIR)

clean-debug:
	rm -rf $(DEBUG_BUILD_DIR)

clean-deb:
	rm -rf $(DEB_BUILD_DIR) $(DEB_OUTPUT_DIR)

distclean:
	rm -rf build build-* build_*
