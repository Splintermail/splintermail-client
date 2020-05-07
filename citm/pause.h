// a callback with a condition check
struct pause_t;
typedef struct pause_t pause_t;

struct pause_t {
    // you can't call run until ready returns true
    bool (*ready)(pause_t*);
    // you must call either run or cancel, but not both
    derr_t (*run)(pause_t**);
    void (*cancel)(pause_t**);
};
