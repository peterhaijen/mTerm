BUILD_DIR := build/release
GENERATOR ?= Ninja

.PHONY: help release run tag push clean clean-release distclean

help:
	@echo "Available targets:"
	@echo "  release        Configure and build a release binary in $(BUILD_DIR)"
	@echo "  run            Run the most recently built mTerm executable"
	@echo "  tag            Create a new release tag"
	@echo "  push           Push the current branch and tags"
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

distclean:
	rm -rf build build-* build_*
