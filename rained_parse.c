#include "rained_parse.h"
#include "rained.h"

typedef enum
{
    lex_token_idk,
    lex_token_keyword,
    lex_token_id,
    lex_token_constant,
    lex_token_string_literal,
    lex_token_punctuator,
    lex_token_comment,

} rained_lex_token;

typedef struct
{
    string              text;
    u32                 p;
    u32                 token_start;
    rained_lex_token    token;
    b8                  quit;

} rained_lexer;


internal b8 char_is_digit(c8 c)
{
    return c >= '0' && c <= '9';
}

internal b8 char_is_letter(c8 c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

internal b8 char_is_alpha(c8 c)
{
    return char_is_digit(c) || char_is_letter(c);
}

internal b8 char_is_whitespace(c8 c)
{
    switch(c)
    {
        case ' ':
        case '\r':
        case '\n':
        return 1;
    }
    return 0;
}

static const string c_keywords[] = 
{
    string_literal("auto"),
    string_literal("break"),
    string_literal("case"),
    string_literal("char"),
    string_literal("const"),
    string_literal("continue"),
    string_literal("default"),
    string_literal("do"),
    string_literal("double"),
    string_literal("else"),
    string_literal("enum"),
    string_literal("extern"),
    string_literal("float"),
    string_literal("for"),
    string_literal("goto"),
    string_literal("if"),
    string_literal("inline"),
    string_literal("int"),
    string_literal("long"),
    string_literal("register"),
    string_literal("restrict"),
    string_literal("return"),
    string_literal("short"),
    string_literal("signed"),
    string_literal("sizeof"),
    string_literal("static"),
    string_literal("struct"),
    string_literal("switch"),
    string_literal("typedef"),
    string_literal("union"),
    string_literal("unsigned"),
    string_literal("void"),
    string_literal("volatile"),
    string_literal("while"),
    string_literal("_Bool"),
    string_literal("_Complex"),
    string_literal("_Imaginary")
};


static const string c_punctuators[] = 
{
    string_literal("["),
    string_literal("]"),
    string_literal("("),
    string_literal(")"),
    string_literal("{"),
    string_literal("}"),
    string_literal("."),
    string_literal("->"),
    string_literal("++"),
    string_literal("--"),
    string_literal("&"),
    string_literal("*"),
    string_literal("+"),
    string_literal("-"),
    string_literal("~"),
    string_literal("!"),
    string_literal("/"),
    string_literal("%"),
    string_literal("<<"),
    string_literal(">>"),
    string_literal("<"),
    string_literal(">"),
    string_literal("<="),
    string_literal(">="),
    string_literal("=="),
    string_literal("!="),
    string_literal("^"),
    string_literal("|"),
    string_literal("&&"),
    string_literal("||"),
    string_literal("?"),
    string_literal(":"),
    string_literal(";"),
    string_literal("..."),
    string_literal("="),
    string_literal("*="),
    string_literal("/="),
    string_literal("%="),
    string_literal("+="),
    string_literal("-="),
    string_literal("<<="),
    string_literal(">>="),
    string_literal("&="),
    string_literal("^="),
    string_literal("|="),
    string_literal(","),
    string_literal("#"),
    string_literal("##"),
    string_literal("<:"),
    string_literal(":>"),
    string_literal("<%"),
    string_literal("%>"),
    string_literal("%:"),
    string_literal("%:%:"),
};

internal void rained_lex_next(rained_lexer *lexer)
{
    b8 skip_whitespace = 1;
    c8 first_char;
    b8 comment = 0;
    b8 multiline_comment = 0;

    lexer->token_start = lexer->p;

    while(lexer->p < lexer->text.length)
    {
        c8 c = lexer->text.p[lexer->p];
        c8 c_1 = lexer->p >= 1 ? lexer->text.p[lexer->p - 1] : 0;
        c8 c_2 = lexer->p >= 2 ? lexer->text.p[lexer->p - 2] : 0;

        if(skip_whitespace)
        {
            if(char_is_whitespace(c))
            {
                lexer->p++;
                lexer->token_start = lexer->p;
            }
            else
            {
                skip_whitespace = 0;
                first_char = c;
            }
        }
        else if(comment)
        {
            if(c == '\r' || c == '\n')
            {
                lexer->token = lex_token_comment;
                return;
            }
            lexer->p++;
        }
        else if(multiline_comment)
        {
            if(c_2 == '*' && c_1 == '/')
            {
                lexer->token = lex_token_comment;
                return;   
            }
            lexer->p++;
        }
        else
        {
            c8 first_char = lexer->text.p[lexer->token_start];

            if(char_is_letter(first_char))
            {
                if(!char_is_alpha(c))
                {
                    // it's either id or a keyword

                    for(u32 i = 0; i < lengthof(c_keywords); i++)
                    {
                        string kw = c_keywords[i];
                        string view = (string)
                        {
                            .p = lexer->text.p + lexer->token_start,
                            .length = lexer->p - lexer->token_start,
                        };
                        if(string_match(view, kw))
                        {
                            lexer->token = lex_token_keyword;
                            return;
                        }
                    }

                    lexer->token = lex_token_id;
                    return;
                }
            }
            else if(lexer->p != lexer->token_start)
            {                
                if(char_is_digit(first_char) && !char_is_alpha(c))
                {
                    lexer->token = lex_token_constant;
                    return;
                }
                else
                {
                    if(first_char == '\\')
                    {
                        lexer->p++;
                        skip_whitespace = 1;
                        continue;
                    }

                    if(c == '\r' || c == '\n')
                    {
                        skip_whitespace = 1;
                        continue;
                    }

                    if(c_1 != '\\' || c_1 == c_2)
                    {
                        if(first_char == '\"' && c == '\"')
                        {
                            lexer->p++;
                            lexer->token = lex_token_string_literal;
                            return;
                        }

                        if(first_char == '\'' && c == '\'')
                        {
                            lexer->p++;
                            lexer->token = lex_token_string_literal;
                            return;
                        }
                    }

                    if(c == '/' && first_char == '/')
                    {
                        comment = 1;
                        lexer->p++;
                        continue;
                    }

                    if(c == '*' && first_char == '/')
                    {
                        multiline_comment = 1;
                        lexer->p++;
                        continue;
                    }

                    for(u32 i = 0; i < lengthof(c_punctuators); i++)
                    {
                        string punct = c_punctuators[i];
                        string view = (string)
                        {
                            .p = lexer->text.p + lexer->token_start,
                            .length = punct.length,
                        };
                        if(string_match(view, punct))
                        {
                            lexer->p = lexer->token_start + punct.length;
                            lexer->token = lex_token_punctuator;
                            return;
                        }
                    }
                }
            }

            lexer->p++;
        }
    }

    lexer->quit = 1;
}

internal highlight_token_array rained_tokens_from_buffer(rained_buffer *buffer, arena *arena)
{
    PROFILE_BEGIN("tokens_from_buffer");
    highlight_token_array arr = 
    { 
        .tokens = arena_push(arena, 0, 8)
    };

    rained_lexer lexer = 
    {
        .text = buffer->text_string
    };

    while(1)
    {
        rained_lex_next(&lexer);

        if(lexer.quit) { break; }
        
        highlight_token_kind tk;
        switch(lexer.token)
        {
            case lex_token_idk:                 tk = highlight_token_none; break;
            case lex_token_keyword:             tk = highlight_token_keyword; break;
            case lex_token_id:                  tk = highlight_token_type; break;
            case lex_token_constant:            tk = highlight_token_none; break;
            case lex_token_string_literal:      tk = highlight_token_none; break;
            case lex_token_punctuator:          tk = highlight_token_none; break;
            default:                            tk = highlight_token_none; break;
        }
        
        if(tk)
        {
            highlight_token *t = arena_push_noalign(arena, sizeof(highlight_token));
            *t = (highlight_token)
            {
                .kind = tk,
                .offset = lexer.token_start,
                .length = lexer.p - lexer.token_start
            };
            arr.num_tokens++;
        }
    }

    PROFILE_END();
    return arr;
}