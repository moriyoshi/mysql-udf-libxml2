/*
 * libxml2 UDF 
 */
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <my_global.h>
#include <my_sys.h>
#include <my_tree.h>
#include <m_ctype.h>
#include <m_string.h>        // To get strmov()
#include <mysql.h>

#include <limits.h>
#include <assert.h>
#include <libxml/parser.h>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

#ifdef HAVE_DLOPEN

/* These must be right or mysqld will not find the symbol! */

#ifdef __cplusplus
extern "C" {
#endif

#define DOC_TREE_VALUE(tree, element) \
    DOC_TREE_ELEM_VALUE(ELEMENT_KEY((tree), (element)))

#define DOC_TREE_ELEM_VALUE(pair) \
    *(xmlDocPtr*)((char*)(pair) + sizeof(long))

static pthread_once_t thr_init_once = PTHREAD_ONCE_INIT;
static pthread_key_t thr_ctx_key;

int doc_tree_key_cmp_func(void *arg, const void *key1, const void *key2)
{
    if (*(long*)key1 < *(long*)key2)
        return -1;
    else if (*(long*)key1 > *(long*)key2)
        return 1;
    else
        return 0;
}

void doc_tree_elem_free(void *data, TREE_FREE code, void *arg)
{
    if (code == free_free)
        xmlFreeDoc(DOC_TREE_ELEM_VALUE(data));
}

struct thr_ctx {
    TREE docs;
    long next_doc_id;
};

static void thr_ctx_fini(struct thr_ctx *ctx)
{
    if (!ctx)
        return;
    delete_tree(&ctx->docs);
    free(ctx);
}

static int thr_ctx_init(struct thr_ctx *ctx)
{
    ctx->next_doc_id = 1;
    init_tree(&ctx->docs, 4096, 0, sizeof(xmlDocPtr), &doc_tree_key_cmp_func,
              1, &doc_tree_elem_free, &ctx->docs);
    return 0;
}

static void global_init(void)
{
    if (pthread_key_create(&thr_ctx_key, (void(*)(void*))&thr_ctx_fini)) abort();
}

static struct thr_ctx *thr_ctx(void)
{
    struct thr_ctx *ctx;
    if (pthread_once(&thr_init_once, global_init)) abort();
    ctx = pthread_getspecific(thr_ctx_key);
    if (ctx)
        return ctx;
    ctx = malloc(sizeof(*ctx));
    if (!ctx) abort(); /* must not happen */
    if (thr_ctx_init(ctx)) abort();
    if (pthread_setspecific(thr_ctx_key, ctx)) abort();
    return ctx;
}

/**
 * @param initid    Points to a structure that the init function should fill.
 *        This argument is given to all other functions.
 *        my_bool maybe_null    1 if function can return NULL
 *                Default value is 1 if any of the arguments
 *                is declared maybe_null.
 *        unsigned int decimals    Number of decimals.
 *                Default value is max decimals in any of the
 *                arguments.
 *        unsigned int max_length  Length of string result.
 *                The default value for integer functions is 21
 *                The default value for real functions is 13+
 *                default number of decimals.
 *                The default value for string functions is
 *                the longest string argument.
 *        char *ptr;        A pointer that the function can use.
 *
 * @param args        Points to a structure which contains:
 *        unsigned int arg_count        Number of arguments
 *        enum Item_result *arg_type    Types for each argument.
 *                    Types are STRING_RESULT, REAL_RESULT
 *                    and INT_RESULT.
 *        char **args            Pointer to constant arguments.
 *                    Contains 0 for not constant argument.
 *        unsigned long *lengths;        max string length for each argument
 *        char *maybe_null        Information of which arguments
 *                    may be NULL
 *
 * @param message    Error message that should be passed to the user on fail.
 *        The message buffer is MYSQL_ERRMSG_SIZE big, but one should
 *        try to keep the error message less than 80 bytes long!
 *
 * This function should return 1 if something goes wrong. In this case
 * message should contain something usefull!
 */
my_bool xml_parse_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
    int i;
    struct thr_ctx *ctx = thr_ctx();
    initid->ptr = (void*)ctx;

    if (args->arg_count < 1) {
        strncpy(message, "xml_parse requires at least one argument",
                MYSQL_ERRMSG_SIZE);
        return 1;    
    } else if (args->arg_count > 5) {
        strncpy(message, "xml_parse requires at most four argument",
                MYSQL_ERRMSG_SIZE);
        return 1;    
    }

    switch (args->arg_count) {
    case 5:
        args->arg_type[4] = INT_RESULT;
        args->maybe_null[4] = 0;
    case 4:
        args->arg_type[3] = STRING_RESULT;
        args->maybe_null[3] = 1;
    case 3:
        args->arg_type[2] = STRING_RESULT;
        args->maybe_null[2] = 0;
    case 2:
        args->arg_type[1] = INT_RESULT;
        args->maybe_null[1] = 0;
    case 1:
        args->arg_type[0] = STRING_RESULT;
        args->maybe_null[0] = 0;
    }

    initid->maybe_null = 1;
    initid->decimals = 0;
    initid->max_length = 20;

    return 0;
}

/**
 * Deinit function. This should free all resources allocated by
 * this function.
 * @param initid	Return value from xxxx_init
 */
void xml_parse_deinit(UDF_INIT *initrd)
{
}

/**
 * XML/HTML parse function.
 * @param initid  Structure filled by xxx_init
 * @param args    The same structure as to xxx_init. This structure
 *                  contains values for all parameters.
 *
 *                Note that the functions MUST check and convert all
 *                to the type it wants!  Null values are represented by
 *                a NULL pointer
 * @param result  Possible buffer to save result. At least 255 byte long.
 * @param length  Pointer to length of the above buffer.
 *                In this the function should save the result length
 * @param is_null If the result is null, one should store 1 here.
 * @param error   If something goes fatally wrong one should store 1 here.
 *
 * @return This function should return a value
 */
longlong xml_parse(UDF_INIT *initid, UDF_ARGS *args, char *result,
                   unsigned long *length, char *is_null, char *error)
{
    struct thr_ctx *ctx = (struct thr_ctx *)initid->ptr;
    xmlDocPtr doc = 0;
    const char *base_url = "", *encoding = NULL;
    int options = 0, is_html = 0;
    long id;
    switch (args->arg_count) {
    case 5:
        options = *(longlong*)args->args[4];
        break;
    case 4:
        encoding = args->args[3];
        break;
    case 3:
        base_url = args->args[2];
        break;
    case 2:
        is_html = *(longlong*)args->args[1];
        break;
    }

    if (is_html) {
        doc = htmlReadMemory(args->args[0], args->lengths[0], base_url,
                             encoding, options);
    } else {
        doc = xmlReadMemory(args->args[0], args->lengths[0], base_url,
                            encoding, options);
    }
    if (!doc) {
        //*error = 1;
        return 0;
    }

    id = ctx->next_doc_id++;
    {
        TREE_ELEMENT *new_elem = tree_insert(&ctx->docs, &id, sizeof(id), NULL);
        if (!new_elem)
            return 0;
        DOC_TREE_VALUE(&ctx->docs, new_elem) = doc;
    }
    return id;
}

/**
 * @param initid    Points to a structure that the init function should fill.
 *        This argument is given to all other functions.
 *        my_bool maybe_null    1 if function can return NULL
 *                Default value is 1 if any of the arguments
 *                is declared maybe_null.
 *        unsigned int decimals    Number of decimals.
 *                Default value is max decimals in any of the
 *                arguments.
 *        unsigned int max_length  Length of string result.
 *                The default value for integer functions is 21
 *                The default value for real functions is 13+
 *                default number of decimals.
 *                The default value for string functions is
 *                the longest string argument.
 *        char *ptr;        A pointer that the function can use.
 *
 * @param args        Points to a structure which contains:
 *        unsigned int arg_count        Number of arguments
 *        enum Item_result *arg_type    Types for each argument.
 *                    Types are STRING_RESULT, REAL_RESULT
 *                    and INT_RESULT.
 *        char **args            Pointer to constant arguments.
 *                    Contains 0 for not constant argument.
 *        unsigned long *lengths;        max string length for each argument
 *        char *maybe_null        Information of which arguments
 *                    may be NULL
 *
 * @param message    Error message that should be passed to the user on fail.
 *        The message buffer is MYSQL_ERRMSG_SIZE big, but one should
 *        try to keep the error message less than 80 bytes long!
 *
 * This function should return 1 if something goes wrong. In this case
 * message should contain something usefull!
 */
my_bool xml_select_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
    int i;
    struct thr_ctx *ctx = thr_ctx();
    initid->ptr = (void*)ctx;

    if (args->arg_count != 2) {
        strncpy(message, "xml_select requires exactly two arguments",
                MYSQL_ERRMSG_SIZE);
        return 1;    
    }

    args->arg_type[0] = INT_RESULT;
    args->maybe_null[0] = 0;
    args->arg_type[1] = STRING_RESULT;
    args->maybe_null[1] = 0;

    initid->maybe_null = 1;
    initid->max_length = UINT_MAX;

    return 0;
}

/**
 * Deinit function. This should free all resources allocated by
 * this function.
 * @param initid	Return value from xxxx_init
 */
void xml_select_deinit(UDF_INIT *initrd)
{
}

/**
 * XML node selection function.
 * @param initid  Structure filled by xxx_init
 * @param args    The same structure as to xxx_init. This structure
 *                  contains values for all parameters.
 *
 *                Note that the functions MUST check and convert all
 *                to the type it wants!  Null values are represented by
 *                a NULL pointer
 * @param result  Possible buffer to save result. At least 255 byte long.
 * @param length  Pointer to length of the above buffer.
 *                In this the function should save the result length
 * @param is_null If the result is null, one should store 1 here.
 * @param error   If something goes fatally wrong one should store 1 here.
 *
 * @return This function should return a value
 */
char *xml_select(UDF_INIT *initid, UDF_ARGS *args, char *result,
                 unsigned long *length, char *is_null, char *error)
{
    struct thr_ctx *ctx = (struct thr_ctx *)initid->ptr;
    xmlDocPtr doc = NULL;
    long key = *(longlong*)args->args[0];
    const char *xpath = args->args[1];
    xmlChar *_result = NULL;

    {
        void *_elem = tree_search(&ctx->docs, &key, NULL);
        if (!_elem) {
            *is_null = 1;
            *error = 1;
            return NULL;
        }
        doc = DOC_TREE_ELEM_VALUE(_elem);
        assert(doc);
    }

    {
        xmlXPathContextPtr xpctx = NULL;
        xmlXPathObjectPtr xpo = NULL;

        xpctx = xmlXPathNewContext(doc);
        if (!xpctx) {
            *is_null = 1;
            *error = 1;
            return NULL;
        }
        xpo = xmlXPathEval((xmlChar*)xpath, xpctx);
        if (!xpo) {
            xmlXPathFreeContext(xpctx);
            *is_null = 1;
            *error = 1;
            return NULL;
        }

        _result = xmlXPathCastToString(xpo);

        xmlXPathFreeContext(xpctx);
        xmlXPathFreeObject(xpo);
    }

    if (!_result) {
        *is_null = 1;
        *error = 1;
        return NULL;
    }

    {
        size_t len = strlen(_result);
        if (len > *length)
            result = my_malloc(len + 1,  MYF(MY_WME));
        memcpy(result, _result, len);
        result[len] = '\0';
        *length = len;
    }

    return result;
}

/**
 * @param initid    Points to a structure that the init function should fill.
 *        This argument is given to all other functions.
 *        my_bool maybe_null    1 if function can return NULL
 *                Default value is 1 if any of the arguments
 *                is declared maybe_null.
 *        unsigned int decimals    Number of decimals.
 *                Default value is max decimals in any of the
 *                arguments.
 *        unsigned int max_length  Length of string result.
 *                The default value for integer functions is 21
 *                The default value for real functions is 13+
 *                default number of decimals.
 *                The default value for string functions is
 *                the longest string argument.
 *        char *ptr;        A pointer that the function can use.
 *
 * @param args        Points to a structure which contains:
 *        unsigned int arg_count        Number of arguments
 *        enum Item_result *arg_type    Types for each argument.
 *                    Types are STRING_RESULT, REAL_RESULT
 *                    and INT_RESULT.
 *        char **args            Pointer to constant arguments.
 *                    Contains 0 for not constant argument.
 *        unsigned long *lengths;        max string length for each argument
 *        char *maybe_null        Information of which arguments
 *                    may be NULL
 *
 * @param message    Error message that should be passed to the user on fail.
 *        The message buffer is MYSQL_ERRMSG_SIZE big, but one should
 *        try to keep the error message less than 80 bytes long!
 *
 * This function should return 1 if something goes wrong. In this case
 * message should contain something usefull!
 */
my_bool xml_free_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
    int i;
    struct thr_ctx *ctx = thr_ctx();
    initid->ptr = (void*)ctx;

    if (args->arg_count != 1) {
        strncpy(message, "xml_free requires exactly one argument",
                MYSQL_ERRMSG_SIZE);
        return 1;    
    }

    args->arg_type[0] = INT_RESULT;
    args->maybe_null[0] = 0;

    initid->maybe_null = 1;
    initid->decimals = 0;
    initid->max_length = 20;

    return 0;
}

/**
 * Deinit function. This should free all resources allocated by
 * this function.
 * @param initid	Return value from xxxx_init
 */
void xml_free_deinit(UDF_INIT *initrd)
{
}

/**
 * XML node selection function.
 * @param initid  Structure filled by xxx_init
 * @param args    The same structure as to xxx_init. This structure
 *                  contains values for all parameters.
 *
 *                Note that the functions MUST check and convert all
 *                to the type it wants!  Null values are represented by
 *                a NULL pointer
 * @param result  Possible buffer to save result. At least 255 byte long.
 * @param length  Pointer to length of the above buffer.
 *                In this the function should save the result length
 * @param is_null If the result is null, one should store 1 here.
 * @param error   If something goes fatally wrong one should store 1 here.
 *
 * @return This function should return a value
 */
longlong xml_free(UDF_INIT *initid, UDF_ARGS *args, char *result,
                  unsigned long *length, char *is_null, char *error)
{
    struct thr_ctx *ctx = (struct thr_ctx *)initid->ptr;
    xmlDocPtr doc = NULL;
    long key = *(longlong*)args->args[0];

    return (longlong)!tree_delete(&ctx->docs, &key, sizeof(key), NULL);
}


#ifdef __cplusplus
}
#endif

#endif /* HAVE_DLOPEN */
