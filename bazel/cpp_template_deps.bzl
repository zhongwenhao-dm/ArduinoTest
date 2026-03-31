load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load("@bazel_tools//tools/build_defs/repo:utils.bzl", "maybe")

def cpp_template_deps():
    """Loads dependencies."""
    maybe(
        git_repository,
        name = "dm_core_cpp",
        tag = "0.5.0",
        remote = "https://github.com/deepmirrorinc/CoreCpp.git",
    )
