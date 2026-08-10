# OpenAPI Reference

This is the complete OpenAPI 3.1 contract for the authenticated API used by
axkdeck. The reference is rendered from the same checked-in contract that
native builds and generated TypeScript declarations validate.

<a href="../assets/openapi/openapi-v1.json" download>Download the OpenAPI JSON</a>

<div id="redoc-container"></div>
<script src="../assets/openapi/redoc.standalone.js"></script>
<script>
  Redoc.init(
    "../assets/openapi/openapi-v1.json",
    {
      hideHostname: true,
      nativeScrollbars: true,
      pathInMiddlePanel: true
    },
    document.getElementById("redoc-container")
  );
</script>
