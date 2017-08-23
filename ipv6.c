#include "ipv6.h"
#include "config.h"

#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_STDARG_H
#include <stdarg.h>
#endif

#if PARSE_TRACE
#define tracef(...) printf(__VA_ARGS__)
#else
#define tracef(...)
#endif

#ifdef HAVE__SNPRINTF_S
#define platform_snprintf(buffer, bytes, format, ...) \
    _snprintf_s(buffer, bytes, _TRUNCATE, format, __VA_ARGS__)
#else
#define platform_snprintf(buffer, bytes, format, ...) \
    snprintf(buffer, bytes, format, __VA_ARGS__)
#endif


// Original core address RFC 3513: https://tools.ietf.org/html/rfc3513
// Replacement address RFC 4291: https://tools.ietf.org/html/rfc4291

const uint32_t IPV6_STRING_SIZE =
    sizeof "[1234:1234:1234:1234:1234:1234:1234:1234/128%longinterface]:65535";

typedef enum {
    STATE_NONE              = 0,
    STATE_ADDR_COMPONENT    = 1,
    STATE_V6_SEPARATOR      = 2,
    STATE_ZERORUN           = 3,
    STATE_CIDR              = 4,
    STATE_IFACE             = 5,
    STATE_PORT              = 6,
    STATE_POST_ADDR         = 7,
    STATE_ERROR             = 8,
} state_t;

typedef enum {
    EC_DIGIT                = 0,
    EC_HEX_DIGIT            = 1,
    EC_V4_COMPONENT_SEP     = 2,
    EC_V6_COMPONENT_SEP     = 3,
    EC_CIDR_MASK            = 4,
    EC_IFACE                = 5,
    EC_OPEN_BRACKET         = 6,
    EC_CLOSE_BRACKET        = 7,
    EC_WHITESPACE           = 8,
} eventclass_t;

// Address has zerorun separators
typedef enum {
    FLAG_ZERORUN            = 0x00000001,   // indicates that the zerorun index is set
    FLAG_ERROR              = 0x00000002,   // indicates that an error occurred in parsing
    FLAG_IPV4_EMBEDDING     = 0x00000004,   // indicates that IPv4 embedding has occurred
} flag_t;

typedef struct ipv6_reader_state_t {
    ipv6_address_full_t*        address_full;       // pointer to output address
    const char*                 error_message;      // null unless an error occurs, pointer must be static
    const char*                 input;              // pointer to input buffer
    state_t                     current;            // current state
    int32_t                     input_bytes;        // input buffer length in bytes
    int32_t                     position;           // current position in input buffer
    int32_t                     components;         // index of current component left to right
    int32_t                     token_position;     // position of token for current state
    int32_t                     token_len;          // length in characters of token
    int32_t                     separator;          // separator count
    int32_t                     brackets;           // bracket count, should go to 1 then 0 for [::1] address notation
    int32_t                     zerorun;            // component where run of zeros was begun ::1 would be 0, 1::2 would be 1 
    int32_t                     v4_embedding;       // index where v4_embedding occurred
    int32_t                     v4_octets;          // number of octets provided for the v4 address
    uint32_t                    flags;              // flags recording state
    ipv6_diag_func_t            diag_func;          // callback for diagnostics
    void*                       user_data;          // user data passed to diag callback
} ipv6_reader_state_t;


#if PARSE_TRACE
//--------------------------------------------------------------------------------
static const char* state_str (state_t state)
{
    switch (state) {
        case STATE_NONE:            return "state-none";
        case STATE_ADDR_COMPONENT:  return "state-addr-component";
        case STATE_V6_SEPARATOR:    return "state-v6-separator";
        case STATE_ZERORUN:         return "state-zero-run";
        case STATE_CIDR:            return "state-cidr";
        case STATE_IFACE:           return "state-iface";
        case STATE_PORT:            return "state-port";
        case STATE_POST_ADDR:       return "state-post-addr";
        case STATE_ERROR:           return "state-error";
        default: break;
    }
    return "<unknown>";
}

//--------------------------------------------------------------------------------
static const char* eventclass_str (eventclass_t input)
{
    switch (input) {
        case EC_DIGIT:              return "eventclass-digit";
        case EC_HEX_DIGIT:          return "eventclass-hex-digit";
        case EC_V4_COMPONENT_SEP:   return "eventclass-v4-component-sep";
        case EC_V6_COMPONENT_SEP:   return "eventclass-v6-component-sep";
        case EC_CIDR_MASK:          return "eventclass-cidr-mask";
        case EC_IFACE:              return "eventclass-iface";
        case EC_OPEN_BRACKET:       return "eventclass-open-bracket";
        case EC_CLOSE_BRACKET:      return "eventclass-close-bracket";
        case EC_WHITESPACE:         return "eventclass-whitespace";
        default:
            break;
    }

    return "<unknown>";
}
#endif // PARSE_TRACE

//
// Update the current state logging the transition
//
#define CHANGE_STATE(value) \
    tracef("  * %s -> %s %s:%u\n", \
        state_str(state->current), state_str(value), __FILE__, (uint32_t)__LINE__); \
    state->current = value;

#define BEGIN_TOKEN(offset) \
    tracef("  * %s: token begin at %u\n", state_str(state->current), state->position + offset); \
    state->token_position = state->position + offset; \
    state->token_len = 0; \

//
// Indicate the presence of an invalid event class character for the current state
//
#define INVALID_INPUT() \
    tracef("invalid input class (%d) in state: %s at position %d of '%s' (%c)\n", \
        input, state_str(state->current), state->position, state->input, state->input[state->position]); \
    ipv6_error(state, IPV6_DIAG_INVALID_INPUT, "Invalid input"); \
    return;

//
// Validate a condition in the parser state
//
#define VALIDATE(msg, diag, cond, action) \
    if (!(cond)) { \
        tracef("  failed '!" #cond "' in state: %s at position %d of '%s'\n\n", \
            state_str(state->current), state->position, state->input); \
        ipv6_error(state, diag, msg); \
        action; \
    }

//--------------------------------------------------------------------------------
// Indicate error, function here for breakpoints
static void ipv6_error(ipv6_reader_state_t* state,
    ipv6_diag_event_t event,
    const char* message)
{
    ipv6_diag_info_t info;
    info.message = message;
    info.input = state->input;
    info.position = state->position;

    state->diag_func(event, &info, state->user_data);
    state->flags |= FLAG_ERROR;
    state->error_message = message;
    CHANGE_STATE(STATE_ERROR);
}

//--------------------------------------------------------------------------------
static int32_t read_decimal_token (ipv6_reader_state_t* state)
{
    VALIDATE("Invalid token", 
            IPV6_DIAG_INVALID_DECIMAL_TOKEN,
            state->token_position + state->token_len <= state->input_bytes,
            return 0);

    const char* cp = state->input + state->token_position;
    const char* ep = cp + state->token_len;
    int32_t accumulate = 0;
    int32_t digit;
    while (*cp && cp < ep) {
        switch (*cp) {
            case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
                digit = *cp - '0';
                accumulate = (accumulate * 10) + digit;
                break;

            default:
                ipv6_error(state, IPV6_DIAG_INVALID_INPUT, "Non-decimal in token input");
                return 0;
        }
        cp++;
    }

    return accumulate;
}

//--------------------------------------------------------------------------------
static int32_t read_hexidecimal_token (ipv6_reader_state_t* state)
{
    VALIDATE("Invalid token", 
            IPV6_DIAG_INVALID_HEX_TOKEN,
            state->token_position + state->token_len <= state->input_bytes,
            return 0);

    const char* cp = state->input + state->token_position;
    const char* ep = cp + state->token_len;
    int32_t accumulate = 0;
    int32_t digit;
    while (*cp && cp < ep) {
        switch (*cp) {
            case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
                digit = (*cp - '0');
                break;

            case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
                digit = 10 + (*cp - 'a');
                break;

            case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
                digit = 10 + (*cp - 'A');
                break;

            default:
                ipv6_error(state, IPV6_DIAG_INVALID_INPUT, "Non-hexidecimal token input");
                return 0;
        }
        accumulate = (accumulate << 4) | digit;
        cp++;
    }

    return accumulate;
}

//--------------------------------------------------------------------------------
// Move an address component from the state to the output
static void ipv6_parse_component (ipv6_reader_state_t* state) {
    int32_t component = read_hexidecimal_token(state);

    tracef("  * ipv6 address component %4x (%d)\n", (uint16_t)component, component);
    
    VALIDATE("Only 8 16bit components are allowed",
            IPV6_DIAG_V6_BAD_COMPONENT_COUNT,
            state->components < 8,
            return);

    VALIDATE("IPv6 address components must be <= 65535",
            IPV6_DIAG_V6_COMPONENT_OUT_OF_RANGE,
            component <= 0xffff,
            return);

    state->address_full->address.components[state->components] = (uint16_t)component;
    state->components++;

    state->token_position = 0;
    state->token_len = 0;
}

//--------------------------------------------------------------------------------
static void ipv4_parse_component (ipv6_reader_state_t* state) {
    int32_t component = read_decimal_token(state);

    tracef("  * ipv4 address component %2x (%d)\n", (uint8_t)component, component);

    VALIDATE("Only 4 8bit components are allowed in an IPv4 embedding", 
        IPV6_DIAG_V4_BAD_COMPONENT_COUNT,
        state->v4_octets < 4,
        return);

    VALIDATE("IPv4 address components must be <= 255",
        IPV6_DIAG_V4_COMPONENT_OUT_OF_RANGE,
        component <= 0xff,
        return);
   
    VALIDATE("IPv4 embedding must have at least 32bits",
        IPV6_DIAG_IPV4_REQUIRED_BITS,
        state->v4_embedding <= 6,
        return);

    uint8_t* embedding = (uint8_t*)&(state->address_full->address.components[state->v4_embedding]);
      
    embedding[state->v4_octets] = (uint8_t)component;
    state->v4_octets++;

    state->token_position = 0;
    state->token_len = 0;
}

//--------------------------------------------------------------------------------
static void ipvx_parse_component (ipv6_reader_state_t* state) {
    if (state->flags & FLAG_IPV4_EMBEDDING) {
        ipv4_parse_component(state);
    } else {
        ipv6_parse_component(state);
    }
}

//--------------------------------------------------------------------------------
static void ipvx_parse_cidr (ipv6_reader_state_t* state) {
    int32_t mask = read_decimal_token(state);

    VALIDATE("CIDR mask must be between 0 and 128 bits",
        IPV6_DIAG_INVALID_CIDR_MASK, 
        mask > -1 && mask < 129,
        return);

    state->address_full->mask = (uint32_t)mask;
    state->address_full->flags |= IPV6_FLAG_HAS_MASK;
}

//--------------------------------------------------------------------------------
static void ipvx_parse_port (ipv6_reader_state_t* state) {
    int32_t port = read_decimal_token(state);

    VALIDATE("Port must be between 0 and 65535",
        IPV6_DIAG_INVALID_PORT,
        port > -1 && port <= 0xffff,
        return);

    state->address_full->port = (uint16_t)port;
    state->address_full->flags |= IPV6_FLAG_HAS_PORT;
}

//--------------------------------------------------------------------------------
//
// State transition function for parser, given a current state and a event class input
// the state will be updated for a next state or accumulate data within the current state
//
static void ipv6_state_transition (
    ipv6_reader_state_t* state,
    eventclass_t input)
{
    tracef("  * transition input: %s <- %s\n", state_str(state->current), eventclass_str(input));

    switch (state->current) {
        default:
        case STATE_ERROR:
            break;

        case STATE_NONE:
            switch (input) {
                case EC_DIGIT:
                case EC_HEX_DIGIT:
                    CHANGE_STATE(STATE_ADDR_COMPONENT);
                    BEGIN_TOKEN(0);
                    state->token_len++;
                    break;

                case EC_OPEN_BRACKET:
                    VALIDATE("Only one set of balanced brackets are allowed",
                        IPV6_DIAG_INVALID_BRACKETS,
                        state->brackets == 1,
                        return);
                    break;

                case EC_CLOSE_BRACKET:
                    CHANGE_STATE(STATE_POST_ADDR);
                    break;

                case EC_V6_COMPONENT_SEP:
                    CHANGE_STATE(STATE_V6_SEPARATOR);
                    break;

                case EC_CIDR_MASK:
                    CHANGE_STATE(STATE_CIDR);
                    BEGIN_TOKEN(1);
                    break;

                case EC_WHITESPACE:
                    break;

                default:
                    INVALID_INPUT();
                    break;
            }
            break;
    
        case STATE_ADDR_COMPONENT:
            switch (input) {
                case EC_DIGIT:
                case EC_HEX_DIGIT:
                    state->token_len++;
                    break;

                case EC_CLOSE_BRACKET:
                    ipvx_parse_component(state);
                    CHANGE_STATE(STATE_POST_ADDR);
                    break;

                case EC_WHITESPACE:
                    ipvx_parse_component(state);
                    CHANGE_STATE(STATE_NONE);
                    break;

                case EC_V6_COMPONENT_SEP:
                    VALIDATE("IPv4 embedding only allowed in last 32 address bits",
                        IPV6_DIAG_IPV4_INCORRECT_POSITION,
                        (state->flags & FLAG_IPV4_EMBEDDING) == 0,
                        return);
                    ipvx_parse_component(state);
                    CHANGE_STATE(STATE_V6_SEPARATOR);
                    break;

                case EC_V4_COMPONENT_SEP:
                    // Mark the embedding point, don't allow IPv6 address components after this point
                    if (!(state->flags & FLAG_IPV4_EMBEDDING)) {
                        state->v4_embedding = state->components;
                        state->flags |= FLAG_IPV4_EMBEDDING;

                        VALIDATE("IPv4 embedding requires 32 bits of address space",
                            IPV6_DIAG_IPV4_REQUIRED_BITS,
                            state->components < 7,
                            return);

                        // Reserve the components
                        state->components += 2;
                    }
                    ipvx_parse_component(state);

                    // There is no separate state for IPv4 component separators
                    CHANGE_STATE(STATE_NONE);
                    break;

                case EC_IFACE:
                    ipvx_parse_component(state);
                    CHANGE_STATE(STATE_IFACE);
                    break;

                case EC_CIDR_MASK:
                    ipvx_parse_component(state);
                    CHANGE_STATE(STATE_CIDR);
                    BEGIN_TOKEN(1);
                    break;

                default:
                    INVALID_INPUT();
                    break;

            }
            break;

        case STATE_V6_SEPARATOR:
            switch (input) {
                case EC_V6_COMPONENT_SEP:
                    // Second component separator
                    VALIDATE("Only one abbreviation of zeros is allowed",
                        IPV6_DIAG_INVALID_ABBREV,
                        (state->flags & FLAG_ZERORUN) == 0,
                        return)

                    // Mark the position of the run
                    state->zerorun = state->components;
                    state->flags |= FLAG_ZERORUN;

                    tracef("  * zero run index: %d\n", state->zerorun);

                    CHANGE_STATE(STATE_NONE);
                    break;

                case EC_WHITESPACE: 
                    CHANGE_STATE(STATE_NONE);
                    break;

                case EC_DIGIT:
                case EC_HEX_DIGIT:
                    CHANGE_STATE(STATE_ADDR_COMPONENT);
                    BEGIN_TOKEN(0);
                    state->token_len++;
                    break;
                
                case EC_IFACE:
                    CHANGE_STATE(STATE_IFACE);
                    break;

                case EC_CIDR_MASK:
                    BEGIN_TOKEN(0);
                    CHANGE_STATE(STATE_CIDR);
                    BEGIN_TOKEN(1);
                    break;

                default:
                    INVALID_INPUT();
                    break;
            }
            break;

        case STATE_IFACE:
            // TODO: identify all valid interface characters 
            switch (input) {
                case EC_WHITESPACE:
                    CHANGE_STATE(STATE_NONE);
                    break;

                case EC_CLOSE_BRACKET:
                    CHANGE_STATE(STATE_POST_ADDR);
                    break;

                default:
                    break;
            }
            break;

        case STATE_CIDR:
            switch (input) {
                case EC_DIGIT:
                    state->token_len++;
                    break;
                
                case EC_CLOSE_BRACKET:
                    ipvx_parse_cidr(state);
                    CHANGE_STATE(STATE_POST_ADDR);
                    break;

                case EC_WHITESPACE:
                    ipvx_parse_cidr(state);
                    CHANGE_STATE(STATE_NONE);
                    break;

                case EC_IFACE:
                    ipvx_parse_cidr(state);
                    CHANGE_STATE(EC_IFACE);
                    break;

                default:
                    INVALID_INPUT();
                    break;
            }
            break;

        case STATE_POST_ADDR:
            switch (input) {
                case EC_WHITESPACE:
                    break;

                case EC_V6_COMPONENT_SEP:
                    CHANGE_STATE(STATE_PORT);
                    BEGIN_TOKEN(1); // start the port token after the separator
                    break;

                default:
                    INVALID_INPUT();
                    break;

            }
            break;

        case STATE_PORT:
            switch (input) {
                case EC_DIGIT:
                    state->token_len++;
                    break;

                case EC_WHITESPACE:
                    ipvx_parse_port(state);
                    CHANGE_STATE(STATE_NONE);
                    break;

                default:
                    INVALID_INPUT();
            }
            break;

    } // end state switch
}

//--------------------------------------------------------------------------------
bool IPV6_API_DEF(ipv6_from_str_diag) (
    const char* input,
    size_t input_bytes,
    ipv6_address_full_t* out,
    ipv6_diag_func_t func,
    void* user_data)
{
    const char *cp = input; 
    const char* ep = input + input_bytes;
    ipv6_reader_state_t state = { 0, };

    state.diag_func = func;
    state.user_data = user_data;

    if (!input || !*input || !out) {
        ipv6_error(&state, IPV6_DIAG_INVALID_INPUT,
            "Invalid input");
        return false;
    }

    if (input_bytes > IPV6_STRING_SIZE) {
        ipv6_error(&state, IPV6_DIAG_STRING_SIZE_EXCEEDED,
            "Input string size exceeded");
        return false;
    }

    memset(out, 0, sizeof(ipv6_address_full_t));

    state.current = STATE_NONE;
    state.input = input;
    state.input_bytes = (int32_t)input_bytes;
    state.address_full = out;

    while (*cp && cp < ep) {
        tracef(
            "  * parse state: %s, cp: '%c' (%02x) position: %d, flags: %08x\n",
            state_str(state.current),
            *cp,
            *cp,
            state.position,
            state.flags);

        switch (*cp) {
            case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
                ipv6_state_transition(&state, EC_DIGIT);
                break;

            case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
            case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
                ipv6_state_transition(&state, EC_HEX_DIGIT);
                break;

            case ':':
                ipv6_state_transition(&state, EC_V6_COMPONENT_SEP);
                break;

            case '.':
                ipv6_state_transition(&state, EC_V4_COMPONENT_SEP);
                break;

            case '/':
                ipv6_state_transition(&state, EC_CIDR_MASK);
                break;

            case '%':
                ipv6_state_transition(&state, EC_IFACE);
                break;

            case '[':
                state.brackets++;
                ipv6_state_transition(&state, EC_OPEN_BRACKET);
                break;

            case ']':
                ipv6_state_transition(&state, EC_CLOSE_BRACKET);
                break;

            case ' ':
            case '\t':
            case '\n':
            case '\r':
                ipv6_state_transition(&state, EC_WHITESPACE);
                break;


            default:
                ipv6_error(&state, IPV6_DIAG_INVALID_INPUT_CHAR,
                    "Invalid input character");
                break;
        }

        // Exit the parse if the last state change triggered an error
        if (state.flags & FLAG_ERROR) {
            return false;
        }

        cp++;
        state.position++;
    }

    // Treat the end of input as whitespace to simplify state transitions
    ipv6_state_transition(&state, EC_WHITESPACE);

    // Mark the presence of embedded IPv4 addresses
    if (state.flags & FLAG_IPV4_EMBEDDING) {
        if (state.v4_octets != 4) {
            ipv6_error(&state, IPV6_DIAG_INVALID_IPV4_EMBEDDING,
                    "IPv4 address embedding was used but required 4 octets");
        } else {
            state.address_full->flags |= IPV6_FLAG_IPV4_EMBED;
        }
    }

    // Early out if there was an error processing the string
    if ((state.flags & FLAG_ERROR) != 0) {
        return false;
    }
    
    
    // If there was no abbreviated run all components should be specified
    if ((state.flags & FLAG_ZERORUN) == 0) {
        if (state.components < IPV6_NUM_COMPONENTS) {
            ipv6_error(&state, IPV6_DIAG_V6_BAD_COMPONENT_COUNT,
                "Invalid component count");
            return false;
        }
        return true;
    }

    uint16_t dst[IPV6_NUM_COMPONENTS] = {0, };
    uint16_t* src = out->address.components;

    // Number of components moving
    int32_t move_count = state.components - state.zerorun;
    int32_t target = IPV6_NUM_COMPONENTS - move_count;
    if (move_count < 0 || move_count > IPV6_NUM_COMPONENTS) {
        tracef("invalid move_count: %d\n", move_count);
        return false;
    }
    if (target < 0 || target + move_count > IPV6_NUM_COMPONENTS) {
        tracef("invalid target location: %d:%d\n", target, move_count);
        return false;
    }

    // Copy the right side of the zero run 
    memcpy(&dst[target], &src[state.zerorun], move_count * sizeof(uint16_t));

    // Copy the left side of the zero run
    memcpy(&dst[0], &src[0], state.zerorun * sizeof(uint16_t));
    
    // Everything else is zero, so just copy the destination array into the output directly
    memcpy(&(out->address.components[0]), &dst[0], IPV6_NUM_COMPONENTS * sizeof(uint16_t));

    return true;
}

//--------------------------------------------------------------------------------
static void ipv6_default_diag (
    ipv6_diag_event_t event,
    const ipv6_diag_info_t* info,
    void* user_data)
{
    (void)event;
    (void)info;
    (void)user_data;
}

//--------------------------------------------------------------------------------
bool IPV6_API_DEF(ipv6_from_str) (
    const char* input,
    size_t input_bytes,
    ipv6_address_full_t* out)
{
    return ipv6_from_str_diag(input, input_bytes, out, ipv6_default_diag, NULL);
}

#define OUTPUT_TRUNCATED() \
    tracef("  ! buffer truncated at position %u\n", (uint32_t)(wp - out)); \
    *out = '\0';

//--------------------------------------------------------------------------------
char* IPV6_API_DEF(ipv6_to_str) (
    const ipv6_address_full_t* in,
    char *out,
    size_t size)
{
    if (size < 4) {
        return NULL;
    }

    if (!in) {
        return NULL;
    }

    const uint16_t* components = in->address.components; 
    char* wp = out; // write pointer
    const char* ep = out + size - 1; // end pointer with one octet for nul
    char token[16] = {0, };

    // For each component find the length of 0 digits that it covers (including
    // itself), if that span is the current longest span of 0 digits record the
    // position
    uint32_t spans_position = 0;
    uint32_t longest_span = 0;
    uint32_t longest_position = 0;
    uint8_t spans[IPV6_NUM_COMPONENTS] = { 0, };
    for (uint32_t i = 0; i < IPV6_NUM_COMPONENTS; ++i) {
        if (components[i]) {
            if (spans[spans_position] > longest_span) {
                longest_position = spans_position;
                longest_span = spans[spans_position];
            }
            spans_position = i + 1;
        }
        else {
            spans[spans_position]++;
        }
    }

    // Check the last identified span
    if (spans_position < IPV6_NUM_COMPONENTS && spans[spans_position] > longest_span) {
        longest_position = spans_position;
        longest_span = spans[spans_position];
    }

    // Bracket the address to supply a port
    if (in->flags & IPV6_FLAG_HAS_PORT) {
        *wp++ = '[';
    }

    // Emit all of the components
    for (uint32_t i = 0; i < IPV6_NUM_COMPONENTS; ++i) {
        const char* cp = token;

        // Write out the last two components as the IPv4 embed
        if (i == 6 && in->flags & IPV6_FLAG_IPV4_EMBED) {
            const uint8_t* ipv4 = (const uint8_t*)&components[i];
            platform_snprintf(
                token,
                sizeof(token),
                "%d.%d.%d.%d",
                ipv4[0], ipv4[1], ipv4[2], ipv4[3]);
            i++;
        } else {
            platform_snprintf(
                token,
                sizeof(token),
                "%x",
                components[i]);
        }

        // Skip the longest span of zeros by emitting the double colon abbreviation instead
        // of the token and continuing on the component at the end of the span
        if (i == longest_position && longest_span > 1) {
            if (wp + 2 >= ep) {
                OUTPUT_TRUNCATED();
                return NULL;
            }

            // The previous component already emitted a separator, or this is the
            // the first separator
            if (i > 0) {
                *wp++ = ':';
            } else {
                *wp++ = ':';
                *wp++ = ':';
            }
            i += (longest_span - 1);
            continue;
        } else {
            // Copy the token up to the terminator
            while (wp < ep && *cp) {
                *wp++ = *cp++;
            }

            if (i < IPV6_NUM_COMPONENTS - 1 && wp < ep) {
                *wp++ = ':';
            }
        }

        if (wp == ep) {
            // Truncated, return a deterministic result
            OUTPUT_TRUNCATED();
            return NULL;
        }
    }

    if (in->flags & IPV6_FLAG_HAS_MASK) {
        platform_snprintf(token, sizeof(token), "/%u", in->mask);
        const char* cp = token;
        while (wp < ep && *cp) {
            *wp++ = *cp++;
        }
    }

    if (in->flags & IPV6_FLAG_HAS_PORT) {
        platform_snprintf(token, sizeof(token), "]:%hu", in->port);
        const char* cp = token;
        while (wp < ep && *cp) {
            *wp++ = *cp++;
        }
    }

    *wp = '\0';

    return out;
}

//--------------------------------------------------------------------------------
int32_t IPV6_API_DEF(ipv6_compare) (
    const ipv6_address_full_t* a,
    const ipv6_address_full_t* b)
{
    int32_t compare;

    // First compare the components in order
    for (uint32_t i = 0; i < IPV6_NUM_COMPONENTS; ++i) {
        compare = a->address.components[i] - b->address.components[i];
        if (compare != 0) {
            return compare;
        }
    }

    // Make sure features are the same
    compare = a->flags - b->flags;
    if (compare != 0) {
        return compare;
    }

    // Compare port
    if (a->flags & IPV6_FLAG_HAS_PORT) {
        compare = a->port - b->port;
        if (compare != 0) {
            return compare;
        }
    }

    // Compare mask
    if (a->flags & IPV6_FLAG_HAS_MASK) {
        compare = a->mask - b->mask;
        if (compare != 0) {
            return compare;
        }
    }

    return 0;
}