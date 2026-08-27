print("require('gsusb') via the compiled C binding module...")
local gsusb = require("gsusb")
print("loaded OK, module table:", gsusb)

local ctx, err = gsusb.init()
if not ctx then
    print("gsusb.init() FAILED:", err)
    os.exit(1)
end
print("gsusb.init() -> ctx =", ctx)

gsusb.exit(ctx)
print("gsusb.exit() called OK")

print("SMOKE TEST OK (compiled Lua C module, no FFI needed)")
