#pragma once

#include <cstdint>
#include <string>

// Undefine Windows errno macros to avoid conflicts with our enum
#ifdef EPERM
#undef EPERM
#endif
#ifdef ENOENT
#undef ENOENT
#endif
#ifdef ESRCH
#undef ESRCH
#endif
#ifdef EINTR
#undef EINTR
#endif
#ifdef EIO
#undef EIO
#endif
#ifdef ENXIO
#undef ENXIO
#endif
#ifdef E2BIG
#undef E2BIG
#endif
#ifdef ENOEXEC
#undef ENOEXEC
#endif
#ifdef EBADF
#undef EBADF
#endif
#ifdef ECHILD
#undef ECHILD
#endif
#ifdef EAGAIN
#undef EAGAIN
#endif
#ifdef ENOMEM
#undef ENOMEM
#endif
#ifdef EACCES
#undef EACCES
#endif
#ifdef EFAULT
#undef EFAULT
#endif
#ifdef ENOTBLK
#undef ENOTBLK
#endif
#ifdef EBUSY
#undef EBUSY
#endif
#ifdef EEXIST
#undef EEXIST
#endif
#ifdef EXDEV
#undef EXDEV
#endif
#ifdef ENODEV
#undef ENODEV
#endif
#ifdef ENOTDIR
#undef ENOTDIR
#endif
#ifdef EISDIR
#undef EISDIR
#endif
#ifdef EINVAL
#undef EINVAL
#endif
#ifdef ENFILE
#undef ENFILE
#endif
#ifdef EMFILE
#undef EMFILE
#endif
#ifdef ENOTTY
#undef ENOTTY
#endif
#ifdef ETXTBSY
#undef ETXTBSY
#endif
#ifdef EFBIG
#undef EFBIG
#endif
#ifdef ENOSPC
#undef ENOSPC
#endif
#ifdef ESPIPE
#undef ESPIPE
#endif
#ifdef EROFS
#undef EROFS
#endif
#ifdef EMLINK
#undef EMLINK
#endif
#ifdef EPIPE
#undef EPIPE
#endif
#ifdef EDOM
#undef EDOM
#endif
#ifdef ERANGE
#undef ERANGE
#endif
#ifdef EDEADLK
#undef EDEADLK
#endif
#ifdef ENAMETOOLONG
#undef ENAMETOOLONG
#endif
#ifdef ENOLCK
#undef ENOLCK
#endif
#ifdef ENOSYS
#undef ENOSYS
#endif
#ifdef ENOTEMPTY
#undef ENOTEMPTY
#endif
#ifdef ELOOP
#undef ELOOP
#endif
#ifdef EWOULDBLOCK
#undef EWOULDBLOCK
#endif
#ifdef ENOMSG
#undef ENOMSG
#endif
#ifdef EIDRM
#undef EIDRM
#endif
#ifdef EDEADLOCK
#undef EDEADLOCK
#endif
#ifdef ENOSTR
#undef ENOSTR
#endif
#ifdef ENODATA
#undef ENODATA
#endif
#ifdef ETIME
#undef ETIME
#endif
#ifdef ENOSR
#undef ENOSR
#endif
#ifdef ENOLINK
#undef ENOLINK
#endif
#ifdef EPROTO
#undef EPROTO
#endif
#ifdef EBADMSG
#undef EBADMSG
#endif
#ifdef EOVERFLOW
#undef EOVERFLOW
#endif
#ifdef EILSEQ
#undef EILSEQ
#endif
#ifdef ENOTSOCK
#undef ENOTSOCK
#endif
#ifdef EDESTADDRREQ
#undef EDESTADDRREQ
#endif
#ifdef EMSGSIZE
#undef EMSGSIZE
#endif
#ifdef EPROTOTYPE
#undef EPROTOTYPE
#endif
#ifdef ENOPROTOOPT
#undef ENOPROTOOPT
#endif
#ifdef EPROTONOSUPPORT
#undef EPROTONOSUPPORT
#endif
#ifdef EOPNOTSUPP
#undef EOPNOTSUPP
#endif
#ifdef EAFNOSUPPORT
#undef EAFNOSUPPORT
#endif
#ifdef EADDRINUSE
#undef EADDRINUSE
#endif
#ifdef EADDRNOTAVAIL
#undef EADDRNOTAVAIL
#endif
#ifdef ENETDOWN
#undef ENETDOWN
#endif
#ifdef ENETUNREACH
#undef ENETUNREACH
#endif
#ifdef ENETRESET
#undef ENETRESET
#endif
#ifdef ECONNABORTED
#undef ECONNABORTED
#endif
#ifdef ECONNRESET
#undef ECONNRESET
#endif
#ifdef ENOBUFS
#undef ENOBUFS
#endif
#ifdef EISCONN
#undef EISCONN
#endif
#ifdef ENOTCONN
#undef ENOTCONN
#endif
#ifdef ETIMEDOUT
#undef ETIMEDOUT
#endif
#ifdef ECONNREFUSED
#undef ECONNREFUSED
#endif
#ifdef EALREADY
#undef EALREADY
#endif
#ifdef EINPROGRESS
#undef EINPROGRESS
#endif
#ifdef ECANCELED
#undef ECANCELED
#endif
#ifdef EOWNERDEAD
#undef EOWNERDEAD
#endif
#ifdef ENOTRECOVERABLE
#undef ENOTRECOVERABLE
#endif
#ifdef EHOSTUNREACH
#undef EHOSTUNREACH
#endif

namespace ia64 {
namespace linux {

/**
 * Linux error codes (errno values)
 * These match the standard Linux/POSIX errno values used on IA-64
 */
enum class Errno : int32_t {
    SUCCESS = 0,        // No error
    EPERM = 1,          // Operation not permitted
    ENOENT = 2,         // No such file or directory
    ESRCH = 3,          // No such process
    EINTR = 4,          // Interrupted system call
    EIO = 5,            // I/O error
    ENXIO = 6,          // No such device or address
    E2BIG = 7,          // Argument list too long
    ENOEXEC = 8,        // Exec format error
    EBADF = 9,          // Bad file number
    ECHILD = 10,        // No child processes
    EAGAIN = 11,        // Try again (same as EWOULDBLOCK)
    ENOMEM = 12,        // Out of memory
    EACCES = 13,        // Permission denied
    EFAULT = 14,        // Bad address
    ENOTBLK = 15,       // Block device required
    EBUSY = 16,         // Device or resource busy
    EEXIST = 17,        // File exists
    EXDEV = 18,         // Cross-device link
    ENODEV = 19,        // No such device
    ENOTDIR = 20,       // Not a directory
    EISDIR = 21,        // Is a directory
    EINVAL = 22,        // Invalid argument
    ENFILE = 23,        // File table overflow
    EMFILE = 24,        // Too many open files
    ENOTTY = 25,        // Not a typewriter
    ETXTBSY = 26,       // Text file busy
    EFBIG = 27,         // File too large
    ENOSPC = 28,        // No space left on device
    ESPIPE = 29,        // Illegal seek
    EROFS = 30,         // Read-only file system
    EMLINK = 31,        // Too many links
    EPIPE = 32,         // Broken pipe
    EDOM = 33,          // Math argument out of domain
    ERANGE = 34,        // Math result not representable
    EDEADLK = 35,       // Resource deadlock would occur
    ENAMETOOLONG = 36,  // File name too long
    ENOLCK = 37,        // No record locks available
    ENOSYS = 38,        // Function not implemented
    ENOTEMPTY = 39,     // Directory not empty
    ELOOP = 40,         // Too many symbolic links encountered
    EWOULDBLOCK = 11,   // Operation would block (same as EAGAIN)
    ENOMSG = 42,        // No message of desired type
    EIDRM = 43,         // Identifier removed
    ECHRNG = 44,        // Channel number out of range
    EL2NSYNC = 45,      // Level 2 not synchronized
    EL3HLT = 46,        // Level 3 halted
    EL3RST = 47,        // Level 3 reset
    ELNRNG = 48,        // Link number out of range
    EUNATCH = 49,       // Protocol driver not attached
    ENOCSI = 50,        // No CSI structure available
    EL2HLT = 51,        // Level 2 halted
    EBADE = 52,         // Invalid exchange
    EBADR = 53,         // Invalid request descriptor
    EXFULL = 54,        // Exchange full
    ENOANO = 55,        // No anode
    EBADRQC = 56,       // Invalid request code
    EBADSLT = 57,       // Invalid slot
    EDEADLOCK = 35,     // Resource deadlock (same as EDEADLK)
    EBFONT = 59,        // Bad font file format
    ENOSTR = 60,        // Device not a stream
    ENODATA = 61,       // No data available
    ETIME = 62,         // Timer expired
    ENOSR = 63,         // Out of streams resources
    ENONET = 64,        // Machine is not on the network
    ENOPKG = 65,        // Package not installed
    EREMOTE = 66,       // Object is remote
    ENOLINK = 67,       // Link has been severed
    EADV = 68,          // Advertise error
    ESRMNT = 69,        // Srmount error
    ECOMM = 70,         // Communication error on send
    EPROTO = 71,        // Protocol error
    EMULTIHOP = 72,     // Multihop attempted
    EDOTDOT = 73,       // RFS specific error
    EBADMSG = 74,       // Not a data message
    EOVERFLOW = 75,     // Value too large for defined data type
    ENOTUNIQ = 76,      // Name not unique on network
    EBADFD = 77,        // File descriptor in bad state
    EREMCHG = 78,       // Remote address changed
    ELIBACC = 79,       // Can not access a needed shared library
    ELIBBAD = 80,       // Accessing a corrupted shared library
    ELIBSCN = 81,       // .lib section in a.out corrupted
    ELIBMAX = 82,       // Attempting to link in too many shared libraries
    ELIBEXEC = 83,      // Cannot exec a shared library directly
    EILSEQ = 84,        // Illegal byte sequence
    ERESTART = 85,      // Interrupted system call should be restarted
    ESTRPIPE = 86,      // Streams pipe error
    EUSERS = 87,        // Too many users
    ENOTSOCK = 88,      // Socket operation on non-socket
    EDESTADDRREQ = 89,  // Destination address required
    EMSGSIZE = 90,      // Message too long
    EPROTOTYPE = 91,    // Protocol wrong type for socket
    ENOPROTOOPT = 92,   // Protocol not available
    EPROTONOSUPPORT = 93, // Protocol not supported
    ESOCKTNOSUPPORT = 94, // Socket type not supported
    EOPNOTSUPP = 95,    // Operation not supported on transport endpoint
    EPFNOSUPPORT = 96,  // Protocol family not supported
    EAFNOSUPPORT = 97,  // Address family not supported by protocol
    EADDRINUSE = 98,    // Address already in use
    EADDRNOTAVAIL = 99, // Cannot assign requested address
    ENETDOWN = 100,     // Network is down
    ENETUNREACH = 101,  // Network is unreachable
    ENETRESET = 102,    // Network dropped connection because of reset
    ECONNABORTED = 103, // Software caused connection abort
    ECONNRESET = 104,   // Connection reset by peer
    ENOBUFS = 105,      // No buffer space available
    EISCONN = 106,      // Transport endpoint is already connected
    ENOTCONN = 107,     // Transport endpoint is not connected
    ESHUTDOWN = 108,    // Cannot send after transport endpoint shutdown
    ETOOMANYREFS = 109, // Too many references: cannot splice
    ETIMEDOUT = 110,    // Connection timed out
    ECONNREFUSED = 111, // Connection refused
    EHOSTDOWN = 112,    // Host is down
    EHOSTUNREACH = 113, // No route to host
    EALREADY = 114,     // Operation already in progress
    EINPROGRESS = 115,  // Operation now in progress
    ESTALE = 116,       // Stale file handle
    EUCLEAN = 117,      // Structure needs cleaning
    ENOTNAM = 118,      // Not a XENIX named type file
    ENAVAIL = 119,      // No XENIX semaphores available
    EISNAM = 120,       // Is a named type file
    EREMOTEIO = 121,    // Remote I/O error
    EDQUOT = 122,       // Quota exceeded
    ENOMEDIUM = 123,    // No medium found
    EMEDIUMTYPE = 124,  // Wrong medium type
    ECANCELED = 125,    // Operation Canceled
    ENOKEY = 126,       // Required key not available
    EKEYEXPIRED = 127,  // Key has expired
    EKEYREVOKED = 128,  // Key has been revoked
    EKEYREJECTED = 129, // Key was rejected by service
    EOWNERDEAD = 130,   // Owner died
    ENOTRECOVERABLE = 131, // State not recoverable
    ERFKILL = 132,      // Operation not possible due to RF-kill
    EHWPOISON = 133     // Memory page has hardware error
};

/**
 * Convert errno to human-readable string
 * 
 * @param err Error code
 * @return String description of error
 */
inline const char* ErrnoToString(Errno err) {
    switch (err) {
        case Errno::SUCCESS: return "Success";
        case Errno::EPERM: return "Operation not permitted";
        case Errno::ENOENT: return "No such file or directory";
        case Errno::ESRCH: return "No such process";
        case Errno::EINTR: return "Interrupted system call";
        case Errno::EIO: return "I/O error";
        case Errno::ENXIO: return "No such device or address";
        case Errno::E2BIG: return "Argument list too long";
        case Errno::ENOEXEC: return "Exec format error";
        case Errno::EBADF: return "Bad file number";
        case Errno::ECHILD: return "No child processes";
        case Errno::EAGAIN: return "Try again";
        case Errno::ENOMEM: return "Out of memory";
        case Errno::EACCES: return "Permission denied";
        case Errno::EFAULT: return "Bad address";
        case Errno::ENOTBLK: return "Block device required";
        case Errno::EBUSY: return "Device or resource busy";
        case Errno::EEXIST: return "File exists";
        case Errno::EXDEV: return "Cross-device link";
        case Errno::ENODEV: return "No such device";
        case Errno::ENOTDIR: return "Not a directory";
        case Errno::EISDIR: return "Is a directory";
        case Errno::EINVAL: return "Invalid argument";
        case Errno::ENFILE: return "File table overflow";
        case Errno::EMFILE: return "Too many open files";
        case Errno::ENOTTY: return "Not a typewriter";
        case Errno::ETXTBSY: return "Text file busy";
        case Errno::EFBIG: return "File too large";
        case Errno::ENOSPC: return "No space left on device";
        case Errno::ESPIPE: return "Illegal seek";
        case Errno::EROFS: return "Read-only file system";
        case Errno::EMLINK: return "Too many links";
        case Errno::EPIPE: return "Broken pipe";
        case Errno::EDOM: return "Math argument out of domain";
        case Errno::ERANGE: return "Math result not representable";
        case Errno::EDEADLK: return "Resource deadlock would occur";
        case Errno::ENAMETOOLONG: return "File name too long";
        case Errno::ENOLCK: return "No record locks available";
        case Errno::ENOSYS: return "Function not implemented";
        case Errno::ENOTEMPTY: return "Directory not empty";
        case Errno::ELOOP: return "Too many symbolic links encountered";
        case Errno::ENOMSG: return "No message of desired type";
        case Errno::EOVERFLOW: return "Value too large for defined data type";
        case Errno::ENOTCONN: return "Transport endpoint is not connected";
        case Errno::ECONNREFUSED: return "Connection refused";
        case Errno::ETIMEDOUT: return "Connection timed out";
        default: return "Unknown error";
    }
}

/**
 * Convert errno to integer value
 * 
 * @param err Error code
 * @return Integer error code value
 */
inline int32_t ErrnoToInt(Errno err) {
    return static_cast<int32_t>(err);
}

/**
 * Create errno from integer
 * 
 * @param errNum Error number
 * @return Errno enum value
 */
inline Errno IntToErrno(int32_t errNum) {
    return static_cast<Errno>(errNum);
}

} // namespace linux
} // namespace ia64
