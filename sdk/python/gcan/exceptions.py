class GcanError(Exception):
    """Raised when a libgcan_native call fails.

    `.code` is the raw gcan_native_error code from
    driver/gcan_native/include/gcan_native.h (negative int); str(err) is a
    human-readable message that already includes which operation failed.
    Values and meanings match gsusb.GsusbError's `.code` 1:1, so
    exception-handling code written against one works against the other.
    """

    def __init__(self, code, message):
        self.code = code
        super().__init__(message)
