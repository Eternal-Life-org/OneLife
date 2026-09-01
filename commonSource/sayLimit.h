


int getSayLimit( double inAge );


// 就地截断 s 到最多 maxChars 个 UTF-8 字符(maxChars 是字符数,非字节数)。
// 在字符边界截断,保证不切断多字节字符(中文/emoji)。返回截断后的字符数。
// 健壮:非法/孤立的 UTF-8 后续字节按单字节处理,声明的字节数超过字符串
// 实际剩余时不会越界。
int truncateToUTF8CharCount( char *s, int maxChars );
