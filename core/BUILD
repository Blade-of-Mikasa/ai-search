cc_library(
    name = "domain",
    srcs = [
        "src/document_store.cpp",
        "src/domain.cpp",
        "src/evidence.cpp",
        "src/image_store.cpp",
        "src/video_store.cpp",
    ],
    hdrs = glob(["include/rag_core/*.h"]),
    export_incs = ["include"],
    extra_cxxflags = ["-std=c++20", "-Wno-float-equal"],
    visibility = "PUBLIC",
)

cc_library(
    name = "http",
    srcs = [
        "http/src/http_server.cpp",
        "http/src/json.cpp",
        "http/src/service.cpp",
    ],
    hdrs = glob(["http/include/rag_core/*.h"]),
    export_incs = ["http/include"],
    deps = [":domain", "#pthread"],
    extra_cxxflags = ["-std=c++20", "-Wno-float-equal"],
    visibility = "PUBLIC",
)

cc_binary(
    name = "nano_core",
    srcs = ["http/src/server_main.cpp"],
    deps = [":domain", ":http", "#pthread"],
    extra_cxxflags = ["-std=c++20", "-Wno-float-equal"],
)
