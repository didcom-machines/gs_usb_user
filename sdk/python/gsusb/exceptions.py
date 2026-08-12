class GsusbError(Exception):
    """Raised when a libgsusb call fails.

    `.code` is the raw gsusb_error code from driver/include/gsusb.h
    (negative int); str(err) is a human-readable message that already
    includes which operation failed.
    """

    def __init__(self, code, message):
        self.code = code
        super().__init__(message)
