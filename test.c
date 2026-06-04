void caret_move_to_prev_line() {  while(caret_position) { if(text[caret_position] == '\n')
        {
            caret_position--;
            break;
        }
        caret_position--;
    }
}
