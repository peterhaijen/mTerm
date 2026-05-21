BUILD_DIR := build/release
GENERATOR ?= Ninja

.PHONY: help release run clean clean-release distclean

help:
	@echo "Available targets:"
	@echo "  release        Configure and build a release binary in $(BUILD_DIR)"
	@echo "  run            Run the most recently built mTerm executable"
	@echo "  clean          Remove release build outputs"
	@echo "  clean-release  Alias for clean"
	@echo "  distclean      Remove all build directories"

release:
	cmake -S . -B $(BUILD_DIR) -G "$(GENERATOR)" -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD_DIR)

run:
	@exe=$$(find build build-* build_* -type f -name mTerm -perm -111 -printf '%T@ %p\n' 2>/dev/null | sort -n | tail -1 | cut -d' ' -f2-); \
	if [ -z "$$exe" ]; then \
		echo "No built mTerm executable found under build directories." >&2; \
		exit 1; \
	fi; \
	echo "Running $$exe"; \
	"$$exe"

clean: clean-release

clean-release:
	rm -rf $(BUILD_DIR)

distclean:
	rm -rf build build-* build_*
