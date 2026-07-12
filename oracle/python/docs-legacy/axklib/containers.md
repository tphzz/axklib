# Containers

Container APIs are the normal entry point. They open one or many image paths and
return structured success/error results instead of exposing parser tracebacks.

::: axklib.containers
    options:
      members:
        - OpenOptions
        - AxklibContainer
        - AxklibContainerLoadError
        - AxklibContainerLoadResult
        - AxklibContainerUnsupportedError
        - open
        - open_many