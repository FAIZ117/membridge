{
  "targets": [
    {
      "target_name": "membridge",
      "sources": [
        "cc/addon.cc",
        "cc/segment.cc",
        "cc/registry.cc",
        "cc/header.cc",
        "cc/liveness.cc"
      ],
      "cflags_cc": ["-std=c++20", "-fexceptions"],
      "conditions": [
        ["OS=='mac'", {
          "xcode_settings": {
            "CLANG_CXX_LANGUAGE_STANDARD": "c++20",
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
            "OTHER_CPLUSPLUSFLAGS": ["-std=c++20", "-fexceptions"]
          }
        }],
        ["OS=='win'", {
          "msvs_settings": {
            "VCCLCompilerTool": {
              "AdditionalOptions": ["/std:c++20", "/Zc:__cplusplus", "/EHsc"]
            }
          }
        }]
      ]
    }
  ]
}
