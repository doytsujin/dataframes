#include "Analysis.h"

#include "Processing.h"

#include <unordered_map>

#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics.hpp>


template<typename T>
std::common_type_t<T, double> vectorNthElement(std::vector<T> &data, int32_t n)
{
    assert(n >= 0 && n < data.size());
    std::nth_element(data.begin(), data.begin() + n, data.end());
    return data[n];
}

template<typename T>
std::common_type_t<T, double> vectorQuantile(std::vector<T> &data, double q = 0.5)
{
    assert(!data.empty());

    if(q >= 1.0)
        return *std::max_element(data.begin(), data.end());
    if(q <= 0)
        return *std::min_element(data.begin(), data.end());

    q = std::clamp(q, 0.0, 1.0);
    const double n = data.size() * q - 0.5;
    const int n1 = static_cast<int>(std::floor(n));
    const int n2 = static_cast<int>(std::ceil(n));
    const auto t = n - n1;
    std::nth_element(data.begin(), data.begin() + n1, data.end());
    std::nth_element(data.begin() + n1, data.begin() + n2, data.end());
    return lerp<double>(data[n1], data[n2], t);
}


template<arrow::Type::type id>
std::shared_ptr<arrow::Table> countValueTyped(const arrow::Column &column)
{
    using T = typename TypeDescription<id>::ObservedType;
    using ArrowType = typename TypeDescription<id>::ArrowType;
    using Builder = typename TypeDescription<id>::BuilderType;
    std::unordered_map<T, int64_t> valueCounts;

    iterateOver<id>(column,
        [&] (auto &&elem) { valueCounts[elem]++; },
        [] () {});

    auto valueBuilder = makeBuilder(std::static_pointer_cast<ArrowType>(column.field()->type()));
    arrow::Int64Builder countBuilder;

    valueBuilder->Reserve(valueCounts.size());
    countBuilder.Reserve(valueCounts.size());

    for(auto && [value, count] : valueCounts)
    {
        append(*valueBuilder, value);
        append(countBuilder, count);
    }

    if(column.null_count())
    {
        valueBuilder->AppendNull();
        countBuilder.Append(column.null_count());
    }

    auto valueArray = finish(*valueBuilder);
    auto countArray = finish(countBuilder);

    auto valueColumn = std::make_shared<arrow::Column>(arrow::field("value", column.type(), column.null_count()), valueArray);
    auto countColumn = std::make_shared<arrow::Column>(arrow::field("count", countArray->type(), false), countArray);

    return tableFromArrays({valueArray, countArray}, {"value", "count"});
}

template<typename T>
struct Minimum
{
    T accumulator = std::numeric_limits<T>::max();
    static constexpr const char *name = "min";
    static constexpr bool RequiresSample = true;
    void operator() (T elem) { accumulator = std::min<T>(accumulator, elem); }
    void operator() () {}
    auto get() { return accumulator; }
};

template<typename T>
struct Maximum
{
    T accumulator = std::numeric_limits<T>::min();
    static constexpr const char *name = "max";
    static constexpr bool RequiresSample = true;
    void operator() (T elem) { accumulator = std::max<T>(accumulator, elem); }
    void operator() () {}
    auto get() { return accumulator; }
};

// TODO naive implementation, look for something numerically better
template<typename T>
struct Mean
{
    int64_t count = 0;
    double accumulator = 0;
    static constexpr const char *name = "mean";
    static constexpr bool RequiresSample = true;
    void operator() (T elem) { accumulator += elem; count++; }
    void operator() () {}
    auto get() { return accumulator / count; }
};

template<typename T>
struct Median
{
    std::vector<T> values;

    static constexpr const char *name = "median";
    static constexpr bool RequiresSample = true;
    void operator() (T elem) { values.push_back(elem); }
    void operator() () {}
    auto get() { return vectorQuantile(values, 0.5); }
};

template<typename T>
struct Variance
{
    boost::accumulators::accumulator_set<T, boost::accumulators::features<boost::accumulators::tag::variance>> accumulator;

    static constexpr const char *name = "variance";
    static constexpr bool RequiresSample = true;
    void operator() (T elem) { accumulator(elem); }
    void operator() () {}
    auto get() { return boost::accumulators::variance(accumulator); }
};

template<typename T>
struct StdDev : Variance<T>
{
    static constexpr const char *name = "std dev";
    auto get() { return std::sqrt(Variance<T>::get()); }
};

template<typename T>
struct Sum : Variance<T>
{
    T accumulator{};
    static constexpr const char *name = "sum";
    static constexpr bool RequiresSample = false;
    void operator() (T elem) { accumulator += elem; }
    void operator() () {}
    auto get() { return accumulator; }
};


template<arrow::Type::type id, typename Processor>
auto calculateStatScalar(const arrow::Column &column, Processor &p)
{
    iterateOver<id>(column,
        [&] (auto elem) { p(elem); },
        [] {});

    return p.get();
}

template<template <typename> typename Processor>
std::shared_ptr<arrow::Column> calculateStat(const arrow::Column &column)
{
    return visitType(*column.type(), [&](auto id) -> std::shared_ptr<arrow::Column>
    {
        if constexpr(id.value != arrow::Type::STRING)
        {
            using T = typename TypeDescription<id.value>::ValueType;
            Processor<T> p;
            using ResultT = decltype(p.get());

            if(column.length() - column.null_count() <= 0)
                return toColumn(std::vector<std::optional<ResultT>>{std::nullopt}, p.name);

            const auto result = calculateStatScalar<id.value>(column, p);
            return toColumn(std::vector<ResultT>{result}, { p.name });
        }
        else
            throw std::runtime_error("Operation not supported for type " + column.type()->ToString());
    });
}

std::shared_ptr<arrow::Column> calculateQuantile(const arrow::Column &column, double q, std::string name)
{
    // return calculateStat<Median>(column);
    auto v = toJustVector(column);
    return std::visit([&] (auto &vector) -> std::shared_ptr<arrow::Column>
    {
        using VectorType = std::decay_t<decltype(vector)>;
        using T = typename VectorType::value_type;
        if constexpr(std::is_arithmetic_v<T>)
        {
            auto result = vectorQuantile(vector, q);
            return scalarToColumn(result, name);
        }
        else
            throw std::runtime_error(name + " is allowed only for arithmetics type");
    }, v);
}

std::shared_ptr<arrow::Table> countValues(const arrow::Column &column)
{
    return visitType(*column.type(), [&] (auto id)
    {
        return countValueTyped<id.value>(column);
    });
}

std::shared_ptr<arrow::Column> calculateMin(const arrow::Column &column)
{
    return calculateStat<Minimum>(column);
}

std::shared_ptr<arrow::Column> calculateMax(const arrow::Column &column)
{
    return calculateStat<Maximum>(column);
}

std::shared_ptr<arrow::Column> calculateMean(const arrow::Column &column)
{
    return calculateStat<Mean>(column);
}

std::shared_ptr<arrow::Column> calculateMedian(const arrow::Column &column)
{
    return calculateQuantile(column, 0.5, "median");
}

std::shared_ptr<arrow::Column> calculateQuantile(const arrow::Column &column, double q)
{
    return calculateQuantile(column, q, "quantile " + std::to_string(q));
}

std::shared_ptr<arrow::Array> fromMemory(double *data, int32_t dataCount)
{
    arrow::DoubleBuilder builder;
    builder.AppendValues(data, dataCount);
    return finish(builder);
}

std::shared_ptr<arrow::Column> columnFromArray(std::shared_ptr<arrow::Array> array, std::string name)
{
    auto field = arrow::field(name, array->type(), array->null_count());
    return std::make_shared<arrow::Column>(field, array);
}

std::shared_ptr<arrow::Column> calculateVariance(const arrow::Column &column)
{
    return calculateStat<Variance>(column);
}

std::shared_ptr<arrow::Column> calculateStandardDeviation(const arrow::Column &column)
{
    return calculateStat<StdDev>(column);
}

std::shared_ptr<arrow::Column> calculateSum(const arrow::Column &column)
{
    return calculateStat<Sum>(column);
}

double calculateCorrelation(const arrow::Column &xCol, const arrow::Column &yCol)
{
    if(xCol.null_count() >= xCol.length() || yCol.null_count() >= yCol.length())
        return std::numeric_limits<double>::quiet_NaN();

    struct CorrelationStats
    {
        double sumX = 0;
        double sumY = 0;
        double sumXX = 0;
        double sumYY = 0;
        double sumXY = 0;
        int64_t n = 0;

        void addPair(double xVal, double yVal)
        {
            sumX += xVal;
            sumY += yVal;
            sumXX += xVal * xVal;
            sumYY += yVal * yVal;
            sumXY += xVal * yVal;
            n++;
        }

        double correlation() const
        {
            const auto num = n * sumXY - sumX * sumY;
            const auto den = std::sqrt(n * sumXX - (sumX*sumX)) * std::sqrt(n * sumYY - (sumY*sumY));
            return num / den;
        }
    };

    auto stats = visitType(*xCol.type(), [&] (auto id1)
    {
        return visitType(*yCol.type(), [&] (auto id2) -> CorrelationStats
        {
            if constexpr(id1.value != arrow::Type::STRING  &&  id2.value != arrow::Type::STRING)
            {
                CorrelationStats stats;
                iterateOverJustPairs<id1.value, id2.value>(xCol, yCol,
                    [&] (double xVal, double yVal)
                {
                    stats.addPair(xVal, yVal);
                });
                return stats;
            }
            else
                throw std::runtime_error("Correlation not supported on string types");
        });
    });

    return stats.correlation();
}

std::shared_ptr<arrow::Column> calculateCorrelation(const arrow::Table &table, const arrow::Column &column)
{
    if(table.num_rows() != column.length())
        throw std::runtime_error("cannot calculate correlation: mismatched column/table row counts");

    const auto N = table.num_columns();
    std::vector<double> correlationValues;
    correlationValues.resize(N);

    for(int i = 0; i < N; i++)
    {
        const auto columnI = table.column(i);
        const auto isSelfCompare = &column == columnI.get();
        correlationValues[i] = isSelfCompare
            ? 1.0
            : calculateCorrelation(column, *columnI);
    }

    return toColumn(correlationValues, column.name() + "_CORR");
}

std::shared_ptr<arrow::Table> calculateCorrelationMatrix(const arrow::Table &table)
{
    const auto N = table.num_columns();
    std::vector<std::vector<double>> correlationMatrix;
    correlationMatrix.resize(N);
    for(auto &correlationColumn : correlationMatrix)
        correlationColumn.resize(N);

    for(int i = 0; i < N; i++)
    {
        const auto ci = table.column(i);
        correlationMatrix[i][i] = 1.0;
        for(int j = i + 1; j < N; j++)
        {
            const auto cj = table.column(j);
            const auto correlation = calculateCorrelation(*ci, *cj);
            correlationMatrix[i][j] = correlation;
            correlationMatrix[j][i] = correlation;
        }
    }

    std::vector<std::shared_ptr<arrow::Column>> ret;
    for(int i = 0; i < N; i++)
    {
        auto c = toColumn(correlationMatrix.at(i), table.column(i)->name());
        ret.push_back(c);
    }

    return tableFromColumns(ret);
}

double autoCorrelation(const std::shared_ptr<arrow::Column> &column, int lag /*= 1*/)
{
    auto shiftedColumn = shift(column, lag);
    auto debug = toVector<std::optional<int64_t>>(*shiftedColumn);
    return calculateCorrelation(*column, *shiftedColumn);
}

struct Length
{
    int64_t length = 0;
    static constexpr const char *name = "length";
    static constexpr bool RequiresSample = false;

    template<typename T>
    void operator()(T &&)
    {
        length++;
    }
    void operator()()
    {
        length++;
    }

    auto get() { return length; }
};

template<typename T>
struct First
{
    std::optional<T> value;
    static constexpr const char *name = "first";
    static constexpr bool RequiresSample = true;

    void operator()(T t)
    {
        if(!value)
            value = t;
    }
    void operator() () {}
    double get() { return value.value_or(T{}); }
};

template<typename T>
struct Last
{
    std::optional<T> value;
    static constexpr const char *name = "last";
    static constexpr bool RequiresSample = true;

    void operator()(T t)
    {
        value = t;
    }
    void operator() () {}
    double get() { return value.value_or(T{}); }

};

template<typename T>
struct AggregateBase
{
    virtual ~AggregateBase(){}

    virtual void operator()(T t) = 0;
    virtual void operator()() = 0;
    virtual std::optional<double> get(bool hadValidValue) = 0;
    virtual std::string name() = 0;
};

template<typename T, typename Aggregator>
struct AggregateBy : AggregateBase<T>
{
    Aggregator a;

    virtual void operator()(T t) override
    {
        a(t);
    }
    virtual void operator()() override
    {
        a();
    }
    virtual std::optional<double> get(bool hadValidValue) override
    {
        if(hadValidValue || !Aggregator::RequiresSample)
            return a.get();
        return {};
    }
    virtual std::string name() override
    {
        return Aggregator::name;
    }
};

template<typename T>
std::unique_ptr<AggregateBase<T>> makeAggregator(AggregateFunction a)
{
    switch(a)
    {
    case AggregateFunction::Minimum: return std::make_unique<AggregateBy<T, Minimum<T>>>();
    case AggregateFunction::Maximum: return std::make_unique<AggregateBy<T, Maximum<T>>>();
    case AggregateFunction::Mean: return std::make_unique<AggregateBy<T, Mean<T>>>();
    case AggregateFunction::Length: return std::make_unique<AggregateBy<T, Length>>();
    case AggregateFunction::Median: return std::make_unique<AggregateBy<T, Median<T>>>();
    case AggregateFunction::First: return std::make_unique<AggregateBy<T, First<T>>>();
    case AggregateFunction::Last: return std::make_unique<AggregateBy<T, Last<T>>>();
    }
    throw std::runtime_error("not handled aggregate function id: " + std::to_string((int)a));
}

std::string aggregateName(AggregateFunction a)
{
    // not efficent, but should not be called that often
    return makeAggregator<double>(a)->name();
}

template<typename T>
struct Aggregators
{
    bool hadValidValue = false;
    std::vector<std::unique_ptr<AggregateBase<T>>> aggregators;
    Aggregators(const std::vector<AggregateFunction> &toAggregate)
    {
        aggregators = transformToVector(toAggregate, [](auto aggrEnum) { return makeAggregator<T>(aggrEnum); });
    }
    void operator()(T t)
    {
        hadValidValue = true;
        for(auto &a : aggregators)
            (*a)(t);
    }
    void operator()()
    {
        for(auto &a : aggregators)
            (*a)();
    }
};

DFH_EXPORT std::shared_ptr<arrow::Table> abominableGroupAggregate(std::shared_ptr<arrow::Table> table, std::shared_ptr<arrow::Column> keyColumn, std::vector<std::pair<std::shared_ptr<arrow::Column>, std::vector<AggregateFunction>>> toAggregate)
{
    std::vector<std::shared_ptr<arrow::Column>> newColumns;

    visitDataType(keyColumn->type(), [&](auto type)
    {
        using ArrowType = ArrowTypeFromPtr<decltype(type)>;
        constexpr auto keyTypeID = idFromDataPointer<decltype(type)>;
        using KeyT = typename TypeDescription<keyTypeID>::ObservedType;
        if constexpr(keyTypeID == arrow::Type::LIST)
        {
            throw std::runtime_error("not implemented: grouping by column of list type");
        }
        else
        {
            GroupedKeyInfo<ArrowType> groups{*keyColumn};

            const auto groupCount = groups.groupCount();
            const auto hasNulls = groups.hasNulls;
            const auto afterLastGroup = groups.uniqueValues.size()+1;

            std::vector<KeyT> keyValues(afterLastGroup);
            for(auto [keyValue, groupId] : groups.uniqueValues)
                keyValues[groupId] = keyValue;

            // build column with unique key values
            {
                auto builder = makeBuilder(type);
                if(hasNulls)
                    builder->AppendNull();
                for(int group = 1; group < keyValues.size(); ++group)
                    append(*builder, keyValues[group]);

                auto arr = finish(*builder);
                newColumns.push_back(std::make_shared<arrow::Column>(keyColumn->field(), arr));
            }

            // build column for each (column, aggregate function) pair
            for(auto &colAggrs : toAggregate)
            {
                visitType(colAggrs.first->type()->id(), [&](auto id)
                {
                    auto[column, aggregates] = colAggrs;
                    if constexpr(id != arrow::Type::STRING)
                    {
                        using T = typename TypeDescription<id.value>::ObservedType;
                        std::vector<Aggregators<T>> aggregators;
                        aggregators.reserve(afterLastGroup);
                        for(int i = 0; i < afterLastGroup; i++)
                            aggregators.emplace_back(aggregates);

                        int64_t row = 0;
                        iterateOver<id.value>(*column, [&](auto value)
                            {
                                const auto groupId = groups.groupIds[row++];
                                aggregators[groupId](value);
                            },
                            [&]()
                            {
                                const auto groupId = groups.groupIds[row++];
                                aggregators[groupId]();
                            });

                        std::vector<arrow::DoubleBuilder> newColumnBuilders(aggregates.size());
                        for(auto &&newColumnBuilder : newColumnBuilders)
                            newColumnBuilder.Reserve(groupCount);

                        for(int64_t groupItr = !hasNulls; groupItr < afterLastGroup; ++groupItr)
                        {
                            auto &aggr = aggregators[groupItr];
                            for(int32_t i = 0; i < aggregates.size(); i++)
                            {
                                if(auto result = aggr.aggregators[i]->get(aggr.hadValidValue))
                                    newColumnBuilders[i].Append(*result);
                                else
                                    newColumnBuilders[i].AppendNull();
                            }
                        }
                        for(int32_t i = 0; i < aggregates.size(); i++)
                        {
                            auto arr = finish(newColumnBuilders[i]);
                            auto col = toColumn(arr, column->name() + "_"s + aggregateName(aggregates[i]));
                            newColumns.push_back(col);
                        };
                    }
                    else
                        throw std::runtime_error("cannot aggregate column `" + column->name() +  "` of type " + column->type()->ToString() + ": must be numeric type!");
                });
            }
        }
    });



    return tableFromColumns(newColumns);
}

