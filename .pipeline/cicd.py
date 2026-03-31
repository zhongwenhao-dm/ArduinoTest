from goar.argo import DAGTemplate
from goar.extensions.cicd import CICDContext
from goar.image import DevImage
from goar.well_known import create_bazel_build_task, create_sanitizer_test_task


def build_pipeline(context: CICDContext) -> DAGTemplate:
    build_dev_image_task, dev_image_url = DevImage(
        depend_files=[r"^artifacts/docker/.*$"]
    ).build(context)
    build_test_task = create_bazel_build_task(
        "build-and-test", dev_image_url, context.source_code_volume
    ).depend(build_dev_image_task)
    sanitizer_test_task = create_sanitizer_test_task(
        "sanitizer-test", dev_image_url, context.source_code_volume
    ).depend(build_dev_image_task)

    return context.make_pipeline(
        tasks=[build_dev_image_task, build_test_task, sanitizer_test_task]
    )
