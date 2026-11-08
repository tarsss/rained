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
    lex_token_pp_directive,

} rained_lex_token_kind;

typedef struct
{
    string                  text;
    u32                     p;
    u32                     token_start;
    rained_lex_token_kind   token;
    rained_lex_token_kind   prev_token;
    b8                      quit;
    b8                      could_not_be_pp;
    b8                      pp;

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
    string_lit("auto"),         string_lit("break"),        string_lit("case"),
    string_lit("char"),         string_lit("const"),        string_lit("continue"),
    string_lit("default"),      string_lit("do"),           string_lit("double"),
    string_lit("else"),         string_lit("enum"),         string_lit("extern"),
    string_lit("float"),        string_lit("for"),          string_lit("goto"),
    string_lit("if"),           string_lit("inline"),       string_lit("int"),
    string_lit("long"),         string_lit("register"),     string_lit("restrict"),
    string_lit("return"),       string_lit("short"),        string_lit("signed"),
    string_lit("sizeof"),       string_lit("static"),       string_lit("struct"),
    string_lit("switch"),       string_lit("typedef"),      string_lit("union"),
    string_lit("unsigned"),     string_lit("void"),         string_lit("volatile"),
    string_lit("while"),        string_lit("_Bool"),        string_lit("_Complex"),
    string_lit("_Imaginary")
};


static const string c_punctuators[] = 
{
    string_lit("["),    string_lit("]"),    string_lit("("),
    string_lit(")"),    string_lit("{"),    string_lit("}"),
    string_lit("."),    string_lit("->"),   string_lit("++"),
    string_lit("--"),   string_lit("&"),    string_lit("*"),
    string_lit("+"),    string_lit("-"),    string_lit("~"),
    string_lit("!"),    string_lit("/"),    string_lit("%"),
    string_lit("<<"),   string_lit(">>"),   string_lit("<"),
    string_lit(">"),    string_lit("<="),   string_lit(">="),
    string_lit("=="),   string_lit("!="),   string_lit("^"),
    string_lit("|"),    string_lit("&&"),   string_lit("||"),
    string_lit("?"),    string_lit(":"),    string_lit(";"),
    string_lit("..."),  string_lit("="),    string_lit("*="),
    string_lit("/="),   string_lit("%="),   string_lit("+="),
    string_lit("-="),   string_lit("<<="),  string_lit(">>="),
    string_lit("&="),   string_lit("^="),   string_lit("|="),
    string_lit(","),    string_lit("#"),    string_lit("##"),
    string_lit("<:"),   string_lit(":>"),   string_lit("<%"),
    string_lit("%>"),   string_lit("%:"),   string_lit("%:%:"),
};

/*
parse <asdfsdf> as pp string literal or whatever
constant parsing, e.g 0.0f, -1
literal parsing e.g L"asdf"
*/
internal void rained_lex_next(rained_lexer *lexer)
{
    b8 skip_whitespace = 1;
    c8 first_char;
    b8 comment = 0;
    b8 multiline_comment = 0;
    b8 is_pp_directive = 0;
    b8 is_header_name = 0;

    lexer->prev_token = lexer->token;
    lexer->token_start = lexer->p;

    while(lexer->p < lexer->text.length)
    {
        c8 c = lexer->text.p[lexer->p];
        c8 c_1 = lexer->p >= 1 ? lexer->text.p[lexer->p - 1] : 0;
        c8 c_2 = lexer->p >= 2 ? lexer->text.p[lexer->p - 2] : 0;

        if(skip_whitespace)
        {
            switch(c)
            {
                case '\r':
                case '\n':
                {
                    lexer->pp = 0;
                    lexer->could_not_be_pp = 0;
                }
                case ' ': 
                {
                    lexer->p++;
                    lexer->token_start = lexer->p;
                    break;
                }
                default:
                {
                    skip_whitespace = 0;
                    first_char = c;
                    break;
                }
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
        else if(is_pp_directive)
        {
            if(!char_is_alpha(c) && char_is_alpha(c_1))
            {
                lexer->token = lex_token_pp_directive;
                lexer->could_not_be_pp = 1;
                return;
            }
            lexer->p++;
        }
        else if(is_header_name)
        {
            if(c_1 == '>')
            {
                lexer->token = lex_token_string_literal;
                lexer->could_not_be_pp = 1;
                return;
            }
            lexer->p++;
        }
        else
        {
            c8 first_char = lexer->text.p[lexer->token_start];
            
            if(!lexer->could_not_be_pp && first_char == '#')
            {
                lexer->pp = 1;
                is_pp_directive = 1;
            }

            if(lexer->pp && lexer->prev_token == lex_token_pp_directive && first_char == '<')
            {
                is_header_name = 1;
                continue;   
            }

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
                            lexer->could_not_be_pp = 1;
                            return;
                        }
                    }

                    lexer->token = lex_token_id;
                    lexer->could_not_be_pp = 1;
                    return;
                }
            }
            else if(lexer->p != lexer->token_start)
            {
                if(char_is_digit(first_char) && !char_is_alpha(c))
                {
                    lexer->token = lex_token_constant;
                    lexer->could_not_be_pp = 1;
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

                    if(c_1 != '\\' || c_1 == c_2)
                    {
                        if(first_char == '\"' && c == '\"')
                        {
                            lexer->p++;
                            lexer->token = lex_token_string_literal;
                            lexer->could_not_be_pp = 1;
                            return;
                        }

                        if(first_char == '\'' && c == '\'')
                        {
                            lexer->p++;
                            lexer->token = lex_token_constant;
                            lexer->could_not_be_pp = 1;
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
                            lexer->could_not_be_pp = 1;
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

typedef struct
{
    rained_lex_token_kind   kind;
    u32                     offset;
    u32                     length;
    
} rained_lex_token;

internal highlight_token_array rained_tokens_from_buffer(rained_buffer *buffer, arena *arena)
{
    PROFILE_BEGIN("tokens_from_buffer");

    rained_lexer lexer = 
    {
        .text = buffer->text_string
    };

    // todo scratch
    rained_lex_token *tokens = arena_push(arena, 0, 8);
    u32 num_tokens = 0;

    while(!lexer.quit)
    {
        rained_lex_next(&lexer);

        rained_lex_token *t = arena_push_struct_noalign(arena, rained_lex_token);
        *t = (rained_lex_token)
        {
            .kind = lexer.token,
            .offset = lexer.token_start,
            .length = lexer.p - lexer.token_start  
        };

        num_tokens++;
    }

    highlight_token_array arr = 
    {
        .tokens = arena_push(arena, 0, 8)
    };

    for(u32 i = 0; i < num_tokens; i++)
    {
        rained_lex_token lt = tokens[i];
        highlight_token_kind tk;

        switch(lt.kind)
        {
            case lex_token_idk:                 tk = highlight_token_none; break;
            case lex_token_keyword:             tk = highlight_token_keyword; break;
            case lex_token_id:                  tk = highlight_token_none; break;
            case lex_token_constant:            tk = highlight_token_constant; break;
            case lex_token_string_literal:      tk = highlight_token_literal; break;
            case lex_token_punctuator:          tk = highlight_token_comment; break;
            case lex_token_comment:             tk = highlight_token_comment; break;
            case lex_token_pp_directive:        tk = highlight_token_pp_directive; break;
            default:                            tk = highlight_token_none; break;
        }
        
        if(tk)
        {
            highlight_token *t = arena_push_noalign(arena, sizeof(highlight_token));
            *t = (highlight_token)
            {
                .kind = tk,
                .offset = lt.offset,
                .length = lt.length
            };
            arr.num_tokens++;
        }
    }

    PROFILE_END();
    return arr;
}