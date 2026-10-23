typedef enum highlight_token_kind
{
    highlight_token_none,
    highlight_token_type,
    highlight_token_function,
    highlight_token_macro,
    highlight_token_comment,
    highlight_token_keyword

} highlight_token_kind;

typedef struct
{
    highlight_token_kind    kind;
    u32                     offset;
    u32                     length;

} highlight_token;

typedef struct
{
    highlight_token *tokens;
    u32             num_tokens;

} highlight_token_array;
